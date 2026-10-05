// Model loading entry point and glTF scene-graph walk.
//
// Split from the rest of the loader along one seam: this file owns everything
// that needs the whole asset (materials, the scene walk, resource lifetime),
// while geometry extraction, GPU upload and animation each live in their own
// translation unit. Four TUs keeps parallel compilation useful without
// scattering one function per file.
//
// Model::ProcessNode is the loader's core: it accumulates node transforms and
// instantiates shared geometry per node.

#include "uhepch.h"
#include "LoadModel.h"
#include <algorithm>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <map>
#include <tuple>
#include "MeshoptDecode.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/Renderer/Renderer.h"
#include "fastgltf/core.hpp"
#include "fastgltf/math.hpp"
#include "fastgltf/types.hpp"
#include "glm/ext/vector_float3.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

namespace UHE::RD3d
{

namespace
{

// Extensions this loader actually honours. Anything else a file declares is
// reported by name instead of being silently ignored.
//
// An extension belongs here only if its VALUES reach the renderer. Parsing a
// field without shading from it is the exact failure this list exists to prevent:
// a file declaring clearcoat renders identical to one without it, but reports no
// warning, so the author has no way to tell.
bool IsExtensionSupported(std::string_view name)
{
    return name == "KHR_materials_pbrSpecularGlossiness" || // SpecularGlossiness path
           name == "KHR_texture_transform" ||               // per-slot UV transform
           name == "KHR_materials_unlit" ||                 // bypasses shading
           name == "KHR_materials_emissive_strength" ||     // emissive multiplier
           name == "KHR_materials_ior" ||                   // dielectric F0
           name == "KHR_materials_clearcoat" ||             // second specular lobe
           name == "KHR_materials_specular" ||              // custom F0 + colour
           name == "KHR_materials_sheen" ||                 // fabric retroreflection
           name == "KHR_materials_transmission" ||          // see-through
           name == "KHR_materials_volume" ||                // medium attenuation
           name == "KHR_materials_iridescence" ||           // thin-film
           name == "KHR_materials_anisotropy" ||            // brushed metal
           name == "KHR_materials_diffuse_transmission" ||  // thin-surface translucency
           name == "KHR_lights_punctual" ||                 // asset-defined lights
           name == "EXT_meshopt_compression" ||             // compressed buffers
           name == "KHR_texture_basisu";                    // KTX2/Basis textures
    // Deliberately absent: KHR_draco_mesh_compression. fastgltf parses its
    // metadata but the mesh cannot be drawn - decoding needs the Draco
    // decoder, which is not vendored - and listing it here would silence
    // exactly the warning that is supposed to explain why such an asset looks
    // broken. Draco-declared primitives additionally carry no readable
    // POSITION, so they draw nothing.
}

} // namespace

Model::~Model()
{
    Destroy();
}

void Model::Destroy()
{
    // Geometry owns every GPU buffer. Mesh entries only borrow, so freeing
    // through m_Geometry releases each buffer exactly once even when several
    // nodes share one glTF mesh.
    ReleaseGeometryBuffers(m_Geometry);
    m_Geometry.clear();
    m_GeometryCache.clear();

    m_LoadedMeshes.clear();
    m_LoadedMaterials.clear();
    m_UnsupportedExtensionNames.clear();
    m_HasUnsupportedExtensions = false;

    m_Animations.clear();
    m_Skeleton.Bones.clear();
    m_Skeleton.JointNodes.clear();
    m_Skeleton.RootBoneID = -1;
    m_Nodes.clear();
    m_RootNodes.clear();
    m_NodeToMesh.clear();
    m_PunctualLights.clear();
}

glm::mat4 Model::NodeLocalTransform(const fastgltf::Node& node)
{
    glm::mat4 local{1.0f};

    std::visit(fastgltf::visitor{[&](const fastgltf::math::fmat4x4& matrix)
                                 { std::memcpy(&local, matrix.data(), sizeof(glm::mat4)); },
                                 [&](const fastgltf::TRS& trs)
                                 {
                                     const glm::vec3 T(trs.translation[0], trs.translation[1], trs.translation[2]);
                                     // glTF stores quaternions as x, y, z, w; glm::quat takes w, x, y, z.
                                     const glm::quat R(trs.rotation[3], trs.rotation[0], trs.rotation[1],
                                                       trs.rotation[2]);
                                     const glm::vec3 S(trs.scale[0], trs.scale[1], trs.scale[2]);
                                     local = glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) *
                                             glm::scale(glm::mat4(1.0f), S);
                                 }},
               node.transform);

    return local;
}

bool Model::loadModel(const std::filesystem::path& filepath, const ModelLoadOptions& options)
{
    if (!std::filesystem::exists(filepath))
    {
        UHE_CORE_ERROR("File not Found {0}", filepath.string());
        return false;
    }

    Destroy();
    m_Options = options;

    static constexpr auto supportedExtensions = ~fastgltf::Extensions::None;
    fastgltf::Parser parser(supportedExtensions);

    constexpr auto gltfOption = fastgltf::Options::DontRequireValidAssetMember | fastgltf::Options::AllowDouble |
                                fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages |
                                fastgltf::Options::GenerateMeshIndices;

    auto gltfFile = fastgltf::MappedGltfFile::FromPath(filepath);
    if (!gltfFile)
    {
        UHE_CORE_ERROR("Failed to open/map glTF file: {0}", fastgltf::getErrorMessage(gltfFile.error()));
        return false;
    }

    auto assetResult = parser.loadGltf(gltfFile.get(), filepath.parent_path(), gltfOption);
    if (assetResult.error() != fastgltf::Error::None)
    {
        UHE_CORE_ERROR("Failed to parse glTF data: {0}", fastgltf::getErrorMessage(assetResult.error()));
        return false;
    }

    fastgltf::Asset asset = std::move(assetResult.get());

    // Record extensions the file declares that this loader does not implement.
    // The parser is constructed with every extension enabled, so an unsupported
    // one is parsed and then ignored - which means the asset renders with subtly
    // wrong shading and no error anywhere. Naming it at load time is the only
    // chance to tell that apart from a shading bug.
    for (const auto& name : asset.extensionsUsed)
    {
        // fastgltf stores these as std::pmr::string; copy into a plain
        // std::string so the loader does not inherit its allocator choice.
        if (!IsExtensionSupported(name))
            m_UnsupportedExtensionNames.emplace_back(name.data(), name.size());
    }
    // extensionsRequired is a hard requirement: the file is invalid without it.
    for (const auto& name : asset.extensionsRequired)
    {
        if (!IsExtensionSupported(name) &&
            std::none_of(m_UnsupportedExtensionNames.begin(), m_UnsupportedExtensionNames.end(),
                         [&](const std::string& existing) { return existing == name.data(); }))
            m_UnsupportedExtensionNames.emplace_back(name.data(), name.size());
    }
    m_HasUnsupportedExtensions = !m_UnsupportedExtensionNames.empty();

    LoadMaterials(asset, filepath);

    ParseSkins(asset);
    ParseAnimations(asset);
    ParsePunctualLights(asset);

    size_t activeSceneIndex = asset.defaultScene.value_or(0);
    if (!asset.scenes.empty() && activeSceneIndex < asset.scenes.size())
    {
        auto& scene = asset.scenes[activeSceneIndex];
        for (auto& rootNodeIndex : scene.nodeIndices)
        {
            m_RootNodes.push_back(static_cast<int>(m_Nodes.size()));
            ProcessNode(asset, rootNodeIndex, glm::mat4{1.0f});
        }
    }

    // Upload happens after the whole scene is walked. Doing it inline per node is
    // what used to produce one duplicate vertex buffer per node; extracting and
    // uploading once per glTF mesh is what fixes it.
    for (size_t i = 0; i < m_Geometry.size(); ++i)
    {
        ComputeBounds(m_Geometry[i]);
        UploadGeometry(m_Geometry[i], i);
    }

    if (m_HasUnsupportedExtensions)
    {
        for (const auto& name : m_UnsupportedExtensionNames)
            UHE_CORE_WARN("glTF declares unimplemented extension '{0}' - shading may be wrong", name);
    }

    return true;
}

namespace
{

// Builds the sampler state glTF declares for a texture.
//
// Defaults match the spec (LINEAR mag, LINEAR_MIPMAP_LINEAR min, REPEAT wrap),
// which is also what the backend used before this existed, so a texture with no
// sampler object renders unchanged. magFilter/minFilter are Optional in fastgltf
// and genuinely absent in many files, so the fallback is the spec default rather
// than a guess.
RHI::SamplerDesc SamplerDescForTexture(const fastgltf::Asset& asset, size_t textureIndex,
                                       RHI::SamplerDesc::ColorSpace colorSpace = RHI::SamplerDesc::ColorSpace::SRGB)
{
    RHI::SamplerDesc desc;
    desc.colorSpace = colorSpace;

    if (textureIndex >= asset.textures.size())
        return desc;

    const auto samplerIndex = asset.textures[textureIndex].samplerIndex;
    if (!samplerIndex.has_value() || *samplerIndex >= asset.samplers.size())
        return desc;

    const fastgltf::Sampler& gltfSampler = asset.samplers[*samplerIndex];

    // A glTF minFilter that selects a mip level only matters when minifying, so
    // every non-Nearest variant maps to Vulkan's eLinear min filter; the mip
    // selection itself is carried by SamplerMipmapMode::eLinear below.
    const auto toFilter = [](fastgltf::Filter f)
    {
        switch (f)
        {
            case fastgltf::Filter::Nearest:
            case fastgltf::Filter::NearestMipMapNearest:
            case fastgltf::Filter::NearestMipMapLinear:
                return RHI::SamplerDesc::Filter::Nearest;
            case fastgltf::Filter::Linear:
            case fastgltf::Filter::LinearMipMapNearest:
            case fastgltf::Filter::LinearMipMapLinear:
            default:
                return RHI::SamplerDesc::Filter::Linear;
        }
    };

    if (gltfSampler.magFilter.has_value())
        desc.magFilter = toFilter(gltfSampler.magFilter.value());
    if (gltfSampler.minFilter.has_value())
        desc.minFilter = toFilter(gltfSampler.minFilter.value());

    // wrapS and wrapT are independent in glTF. Applying one mode to both axes is
    // wrong for any atlas that clamps U but repeats V, which is the common case.
    switch (gltfSampler.wrapS)
    {
        case fastgltf::Wrap::ClampToEdge:
            desc.wrapS = RHI::SamplerDesc::Wrap::ClampToEdge;
            break;
        case fastgltf::Wrap::MirroredRepeat:
            desc.wrapS = RHI::SamplerDesc::Wrap::MirroredRepeat;
            break;
        case fastgltf::Wrap::Repeat:
        default:
            desc.wrapS = RHI::SamplerDesc::Wrap::Repeat;
            break;
    }

    switch (gltfSampler.wrapT)
    {
        case fastgltf::Wrap::ClampToEdge:
            desc.wrapT = RHI::SamplerDesc::Wrap::ClampToEdge;
            break;
        case fastgltf::Wrap::MirroredRepeat:
            desc.wrapT = RHI::SamplerDesc::Wrap::MirroredRepeat;
            break;
        case fastgltf::Wrap::Repeat:
        default:
            desc.wrapT = RHI::SamplerDesc::Wrap::Repeat;
            break;
    }

    return desc;
}

// Converts a glTF KHR_texture_transform into engine-space parameters.
//
// LoadModelGeometry flips V while importing TEXCOORD_0 (glTF measures v from
// the top of the image, the engine from the bottom), so the transform declared
// in the file is NOT valid over the imported coordinates: applying it naively
// mirrors the rotation direction and lands the offset on 1 - offset. Working
// the flip through  uv' = R(theta) * (uv * scale) + offset  in glTF space gives
// the equivalent engine-space transform:
//
//   uv' = R(-theta) * (uv * scale) + (offset.x - sin(theta)*scale.y,
//                                     1 - cos(theta)*scale.y - offset.y)
//
// where R is the 2D rotation matrix. The shader consumes this as
//   uv'.x = cos * scale.x * u + sin * scale.y * v + offset.x
//   uv'.y = -sin * scale.x * u + cos * scale.y * v + offset.y
// with cos/sin of the ORIGINAL angle (R(-theta) spelled out). With no transform
// declared the struct's default (identity) reproduces untouched sampling.
UVTransform EngineUVTransform(const fastgltf::TextureTransform& t)
{
    UVTransform out;
    const float c = std::cos(t.rotation);
    const float s = std::sin(t.rotation);
    out.cosRotation = c;
    out.sinRotation = s;
    out.scale = glm::vec2(t.uvScale.x(), t.uvScale.y());
    out.offset = glm::vec2(t.uvOffset.x() - s * t.uvScale.y(), 1.0f - c * t.uvScale.y() - t.uvOffset.y());
    out.texCoordSet = static_cast<u32>(t.texCoordIndex.value_or(0));
    out.HasTransform = true;
    return out;
}

} // namespace

void Model::LoadMaterials(const fastgltf::Asset& asset, const std::filesystem::path& filepath)
{
    m_LoadedMaterials.resize(asset.materials.size());

    // Per-load texture cache; see the dedup note inside loadTexture.
    std::map<std::tuple<size_t, int, size_t>, Ref<Texture2D>> textureCache;

    for (size_t i = 0; i < asset.materials.size(); ++i)
    {
        auto& gltfMaterial = asset.materials[i];

        const fastgltf::TextureInfo* albedoTextureInfo = nullptr;
        if (gltfMaterial.pbrData.baseColorTexture.has_value())
        {
            albedoTextureInfo = &gltfMaterial.pbrData.baseColorTexture.value();
        }
        else if (gltfMaterial.specularGlossiness && gltfMaterial.specularGlossiness->diffuseTexture.has_value())
        {
            albedoTextureInfo = &gltfMaterial.specularGlossiness->diffuseTexture.value();
        }

        // Colour space is a property of the SLOT, not the file: glTF mandates
        // sRGB for baseColor and emissive, and LINEAR data for normal,
        // metallicRoughness and occlusion. Using one format for all of them
        // gamma-decodes the linear maps on every sample, which is why a normal
        // map can light a surface the wrong way round.
        auto loadTexture = [&](const fastgltf::TextureInfo* texInfo, size_t matIdx,
                               RHI::SamplerDesc::ColorSpace colorSpace =
                                   RHI::SamplerDesc::ColorSpace::SRGB) -> Ref<Texture2D>
        {
            if (!texInfo)
                return nullptr;
            auto textureIndex = texInfo->textureIndex;
            if (textureIndex >= asset.textures.size())
                return nullptr;

            // KHR_texture_basisu (issue #29 Tier 4, #42): when the file ships a
            // KTX2 image it is the PRIMARY representation and imageIndex is the
            // fallback for loaders without basis support. We transcode KTX2, so
            // the basis image wins when present; the fallback still loads when
            // the file carries one, which also keeps GLB-with-PNG-fallback
            // assets rendering if the KTX2 itself fails to decode.
            const auto& gltfTexture = asset.textures[textureIndex];
            auto imageIndex =
                gltfTexture.basisuImageIndex.has_value() ? gltfTexture.basisuImageIndex : gltfTexture.imageIndex;
            if (!imageIndex.has_value())
                return nullptr;

            // Texture dedup (issue #42): one glTF image is frequently bound by
            // several slots and materials (packed metalRough/occlusion images,
            // shared base colours across a foliage set). Without this cache
            // every reference decoded AND uploaded its own GPU texture -
            // identical pixels resident N times. The key includes the colour
            // space (the same image legitimately becomes both an sRGB and a
            // linear texture) and the declared sampler index (a shared image
            // can be bound with different wrap/filter state per texture).
            const auto cacheKey =
                std::make_tuple(*imageIndex, static_cast<int>(colorSpace),
                                gltfTexture.samplerIndex.value_or(std::numeric_limits<size_t>::max()));
            if (const auto found = textureCache.find(cacheKey); found != textureCache.end())
                return found->second;

            auto& image = asset.images[imageIndex.value()];

            // glTF declares sampler state per texture (magFilter/minFilter/wrapS/
            // wrapT). Without this the backend's hardcoded linear/repeat is used
            // for everything, so a CLAMP_TO_EDGE atlas or a NEAREST pixel-art
            // texture samples wrongly.
            RHI::SamplerDesc sampler = SamplerDescForTexture(asset, textureIndex, colorSpace);

            Ref<Texture2D> result = nullptr;
            std::visit(
                fastgltf::visitor{
                    [&](const fastgltf::sources::URI& filePath)
                    {
                        std::filesystem::path imgPath = filepath.parent_path() / filePath.uri.path();
                        result = Texture2D::Create(imgPath.string(), sampler);
                    },
                    [&](const fastgltf::sources::Array& array)
                    {
                        result = Texture2D::CreateFromMemory(array.bytes.data(), array.bytes.size(), sampler);
                        UHE_CORE_INFO("Loaded texture for material {0} from Array, size: {1}", matIdx,
                                      array.bytes.size());
                    },
                    [&](const fastgltf::sources::ByteView& byteView)
                    {
                        result = Texture2D::CreateFromMemory(byteView.bytes.data(), byteView.bytes.size(), sampler);
                        UHE_CORE_INFO("Loaded texture for material {0} from ByteView, size: {1}", matIdx,
                                      byteView.bytes.size());
                    },
                    [&](const fastgltf::sources::Vector& vector)
                    {
                        result = Texture2D::CreateFromMemory(vector.bytes.data(), vector.bytes.size(), sampler);
                        UHE_CORE_INFO("Loaded texture for material {0} from Vector, size: {1}", matIdx,
                                      vector.bytes.size());
                    },
                    [&](const fastgltf::sources::Fallback& fallback)
                    {
                        UHE_CORE_ERROR("fastgltf fallback triggered for material {0} image! Image could not be loaded.",
                                       matIdx);
                    },
                    [&](const fastgltf::sources::BufferView& view)
                    {
                        // The view may be EXT_meshopt_compression-compressed
                        // (meshopt exports compress atlas pages with the
                        // geometry); GetBufferViewBytes handles both cases.
                        std::vector<std::byte> scratch;
                        auto bytes = GetBufferViewBytes(asset, view.bufferViewIndex, scratch);
                        if (bytes.empty())
                        {
                            UHE_CORE_ERROR("BufferView for material {0} has no readable data", matIdx);
                            return;
                        }
                        result = Texture2D::CreateFromMemory(bytes.data(), bytes.size(), sampler);
                        UHE_CORE_INFO("Loaded texture for material {0} from BufferView, size: {1}", matIdx,
                                      bytes.size());
                    },
                    [&](const auto&)
                    { UHE_CORE_ERROR("Unhandled image data type in glTF! Variant index: {0}", image.data.index()); }},
                image.data);
            textureCache.emplace(cacheKey, result);
            return result;
        };

        // Loads a texture into its slot AND records the slot's KHR_texture_transform.
        // Both come off the same TextureInfo, so pairing them here is what keeps a
        // transform from silently diverging from the map it belongs to.
        auto assignSlot = [&](Ref<Texture2D>& target, int slotIndex, const fastgltf::TextureInfo* texInfo,
                              RHI::SamplerDesc::ColorSpace colorSpace = RHI::SamplerDesc::ColorSpace::SRGB)
        {
            if (!texInfo)
                return;
            target = loadTexture(texInfo, i, colorSpace);
            if (texInfo->transform)
            {
                auto& dst = m_LoadedMaterials[i].UVTransforms[slotIndex];
                dst = EngineUVTransform(*texInfo->transform);
                if (dst.texCoordSet != 0)
                {
                    // Only TEXCOORD_0 is extracted (LoadModelGeometry.cpp). Applying
                    // the transform over UV0 is closer to the file's intent than
                    // dropping it, but the author should know the map wanted a
                    // second UV set the engine does not carry.
                    UHE_CORE_WARN("Material {0}: texture transform targets TEXCOORD_{1}; only TEXCOORD_0 exists, "
                                  "applying it to TEXCOORD_0",
                                  i, dst.texCoordSet);
                }
            }
        };

        const int albedoSlot = static_cast<int>(MaterialTextureSlot::Albedo);
        const int mrSlot = static_cast<int>(MaterialTextureSlot::MetallicRoughness);
        const int normalSlot = static_cast<int>(MaterialTextureSlot::Normal);
        const int occlusionSlotIdx = static_cast<int>(MaterialTextureSlot::Occlusion);
        const int emissiveSlotIdx = static_cast<int>(MaterialTextureSlot::Emissive);

        if (albedoTextureInfo)
        {
            assignSlot(m_LoadedMaterials[i].AlbedoTexture, albedoSlot, albedoTextureInfo);
        }

        m_LoadedMaterials[i].MetallicFactor = gltfMaterial.pbrData.metallicFactor;
        m_LoadedMaterials[i].RoughnessFactor = gltfMaterial.pbrData.roughnessFactor;

        // fastgltf already applies the spec defaults for these: baseColorFactor
        // defaults to (1,1,1,1) and emissiveFactor to (0,0,0). Reading them
        // directly is correct - guarding with has_value() would be a type error,
        // and writing our own defaults would only risk disagreeing with fastgltf.
        {
            const auto& f = gltfMaterial.pbrData.baseColorFactor;
            m_LoadedMaterials[i].BaseColorFactor = glm::vec4(f[0], f[1], f[2], f[3]);
        }

        if (gltfMaterial.pbrData.metallicRoughnessTexture.has_value())
        {
            // LINEAR data: metallic (B) and roughness (G) are scalar quantities,
            // not colour. Gamma-decoding them shifts both.
            assignSlot(m_LoadedMaterials[i].MetallicRoughnessTexture, mrSlot,
                       &gltfMaterial.pbrData.metallicRoughnessTexture.value(), RHI::SamplerDesc::ColorSpace::Linear);
        }

        // Normal / occlusion / emissive. fastgltf exposes each as a TextureInfo
        // carrying both the texture index and its own scale/strength scalar, so
        // the map and the scalar come from the same struct.
        if (gltfMaterial.normalTexture.has_value())
        {
            const auto& n = gltfMaterial.normalTexture.value();
            assignSlot(m_LoadedMaterials[i].NormalTexture, normalSlot, &n, RHI::SamplerDesc::ColorSpace::Linear);
            m_LoadedMaterials[i].NormalScale = n.scale;
        }

        if (gltfMaterial.occlusionTexture.has_value())
        {
            const auto& o = gltfMaterial.occlusionTexture.value();
            // Occlusion is a scalar cavity term in the R channel - LINEAR.
            assignSlot(m_LoadedMaterials[i].OcclusionTexture, occlusionSlotIdx, &o,
                       RHI::SamplerDesc::ColorSpace::Linear);
            m_LoadedMaterials[i].OcclusionStrength = o.strength;
        }

        if (gltfMaterial.emissiveTexture.has_value())
        {
            // Emissive is colour, so sRGB - the default the lambda already applies.
            assignSlot(m_LoadedMaterials[i].EmissiveTexture, emissiveSlotIdx, &gltfMaterial.emissiveTexture.value());
        }

        // emissiveFactor defaults to BLACK per spec, which fastgltf applies for
        // us. Defaulting this to white would make every unlit surface glow.
        {
            const auto& e = gltfMaterial.emissiveFactor;
            m_LoadedMaterials[i].EmissiveFactor = glm::vec3(e[0], e[1], e[2]);
        }

        // alphaMode: OPAQUE (spec default) / MASK / BLEND.
        switch (gltfMaterial.alphaMode)
        {
            case fastgltf::AlphaMode::Mask:
                m_LoadedMaterials[i].Alpha = AlphaMode::Mask;
                break;
            case fastgltf::AlphaMode::Blend:
                m_LoadedMaterials[i].Alpha = AlphaMode::Blend;
                m_HasTransparentMaterials = true;
                break;
            case fastgltf::AlphaMode::Opaque:
            default:
                m_LoadedMaterials[i].Alpha = AlphaMode::Opaque;
                break;
        }

        // alphaCutoff only applies to MASK, and defaults to 0.5.
        m_LoadedMaterials[i].AlphaCutoff = gltfMaterial.alphaCutoff;
        m_LoadedMaterials[i].DoubleSided = gltfMaterial.doubleSided;

        // ── Tier 2/3 PBR extensions ─────────────────────────────────────
        //
        // Each is read only when fastgltf actually populated its block. That is
        // the check that matters: fastgltf fills these structs with SPEC DEFAULTS
        // rather than leaving them empty, so reading unconditionally would make
        // every material look like it declared every extension, and a
        // sheenRoughness of 0 or an attenuationDistance of 0 would quietly change
        // how an ordinary PBR surface renders.
        auto& ext = m_LoadedMaterials[i].Extensions;

        // KHR_materials_emissive_strength. A scalar, always populated by the
        // parser, and its default of 1.0 is a no-op multiplier.
        ext.EmissiveStrength = gltfMaterial.emissiveStrength;

        // KHR_materials_ior. Default 1.5, which is the dielectric value.
        ext.IOR = gltfMaterial.ior;

        // KHR_materials_unlit. Tier 3, but it is a bare flag with no data of its
        // own, so it rides along here rather than needing a separate pass.
        ext.Unlit = gltfMaterial.unlit;

        // Optional-carrying variant for the extension maps, which fastgltf
        // stores as Optional<TextureInfo>. Dereferencing an empty optional to
        // get a pointer would be UB before assignSlot's null check could run,
        // so the has_value test has to happen on THIS side of the call.
        auto assignSlotOpt = [&](Ref<Texture2D>& target, int slotIndex, const auto& texInfo,
                                 RHI::SamplerDesc::ColorSpace colorSpace = RHI::SamplerDesc::ColorSpace::SRGB)
        {
            if (!texInfo.has_value())
                return;
            assignSlot(target, slotIndex, &texInfo.value(), colorSpace);
        };

        if (gltfMaterial.clearcoat)
        {
            const auto& cc = *gltfMaterial.clearcoat;
            ext.HasClearcoat = true;
            ext.ClearcoatFactor = cc.clearcoatFactor;
            ext.ClearcoatRoughnessFactor = cc.clearcoatRoughnessFactor;
            if (cc.clearcoatNormalTexture)
                ext.ClearcoatNormalScale = cc.clearcoatNormalTexture->scale;
            // The clearcoat maps are loaded so their textures exist and get a
            // descriptor slot; the shader reads them through the per-material
            // texture slot table.
            assignSlotOpt(ext.ClearcoatTexture, static_cast<int>(MaterialTextureSlot::Clearcoat), cc.clearcoatTexture,
                          RHI::SamplerDesc::ColorSpace::Linear);
            assignSlotOpt(ext.ClearcoatRoughnessTexture, static_cast<int>(MaterialTextureSlot::ClearcoatRoughness),
                          cc.clearcoatRoughnessTexture, RHI::SamplerDesc::ColorSpace::Linear);
            assignSlotOpt(ext.ClearcoatNormalTexture, static_cast<int>(MaterialTextureSlot::ClearcoatNormal),
                          cc.clearcoatNormalTexture, RHI::SamplerDesc::ColorSpace::Linear);
        }

        if (gltfMaterial.specular)
        {
            const auto& sp = *gltfMaterial.specular;
            ext.HasSpecular = true;
            ext.SpecularFactor = sp.specularFactor;
            ext.SpecularColorFactor =
                glm::vec3(sp.specularColorFactor[0], sp.specularColorFactor[1], sp.specularColorFactor[2]);
            assignSlotOpt(ext.SpecularTexture, static_cast<int>(MaterialTextureSlot::Specular), sp.specularTexture,
                          RHI::SamplerDesc::ColorSpace::Linear);
            assignSlotOpt(ext.SpecularColorTexture, static_cast<int>(MaterialTextureSlot::SpecularColor),
                          sp.specularColorTexture);
        }

        if (gltfMaterial.sheen)
        {
            const auto& sh = *gltfMaterial.sheen;
            ext.HasSheen = true;
            ext.SheenColorFactor = glm::vec3(sh.sheenColorFactor[0], sh.sheenColorFactor[1], sh.sheenColorFactor[2]);
            ext.SheenRoughnessFactor = sh.sheenRoughnessFactor;
            assignSlotOpt(ext.SheenColorTexture, static_cast<int>(MaterialTextureSlot::SheenColor),
                          sh.sheenColorTexture);
            assignSlotOpt(ext.SheenRoughnessTexture, static_cast<int>(MaterialTextureSlot::SheenRoughness),
                          sh.sheenRoughnessTexture, RHI::SamplerDesc::ColorSpace::Linear);
        }

        // Transmission and volume are separate extensions but only meaningful
        // together: thickness and attenuation describe the medium a transmitted
        // ray travels through.
        if (gltfMaterial.transmission || gltfMaterial.volume)
        {
            ext.HasTransmission = true;
            if (gltfMaterial.transmission)
            {
                const auto& tr = *gltfMaterial.transmission;
                ext.TransmissionFactor = tr.transmissionFactor;
                assignSlotOpt(ext.TransmissionTexture, static_cast<int>(MaterialTextureSlot::Transmission),
                              tr.transmissionTexture, RHI::SamplerDesc::ColorSpace::Linear);
            }
            if (gltfMaterial.volume)
            {
                const auto& vo = *gltfMaterial.volume;
                ext.ThicknessFactor = vo.thicknessFactor;
                ext.AttenuationDistance = vo.attenuationDistance;
                ext.AttenuationColor =
                    glm::vec3(vo.attenuationColor[0], vo.attenuationColor[1], vo.attenuationColor[2]);
                assignSlotOpt(ext.ThicknessTexture, static_cast<int>(MaterialTextureSlot::Thickness),
                              vo.thicknessTexture, RHI::SamplerDesc::ColorSpace::Linear);
            }
        }

        if (gltfMaterial.iridescence)
        {
            const auto& ir = *gltfMaterial.iridescence;
            ext.HasIridescence = true;
            ext.IridescenceFactor = ir.iridescenceFactor;
            ext.IridescenceIOR = ir.iridescenceIor;
            ext.IridescenceThicknessMinimum = ir.iridescenceThicknessMinimum;
            ext.IridescenceThicknessMaximum = ir.iridescenceThicknessMaximum;
            assignSlotOpt(ext.IridescenceTexture, static_cast<int>(MaterialTextureSlot::Iridescence),
                          ir.iridescenceTexture, RHI::SamplerDesc::ColorSpace::Linear);
            assignSlotOpt(ext.IridescenceThicknessTexture, static_cast<int>(MaterialTextureSlot::IridescenceThickness),
                          ir.iridescenceThicknessTexture, RHI::SamplerDesc::ColorSpace::Linear);
        }

        if (gltfMaterial.anisotropy)
        {
            const auto& an = *gltfMaterial.anisotropy;
            ext.HasAnisotropy = true;
            ext.AnisotropyStrength = an.anisotropyStrength;
            ext.AnisotropyRotation = an.anisotropyRotation;
            assignSlotOpt(ext.AnisotropyTexture, static_cast<int>(MaterialTextureSlot::Anisotropy),
                          an.anisotropyTexture, RHI::SamplerDesc::ColorSpace::Linear);
        }

        // KHR_materials_diffuse_transmission (Tier 3): light diffused through a
        // thin surface - the term that keeps backlit leaves translucent instead
        // of black. Independent of KHR_materials_transmission, which models
        // SPECULAR see-through (glass); a leaf typically declares only this one.
        if (gltfMaterial.diffuseTransmission)
        {
            const auto& dt = *gltfMaterial.diffuseTransmission;
            ext.HasDiffuseTransmission = true;
            ext.DiffuseTransmissionFactor = dt.diffuseTransmissionFactor;
            ext.DiffuseTransmissionColor =
                glm::vec3(dt.diffuseTransmissionColorFactor[0], dt.diffuseTransmissionColorFactor[1],
                          dt.diffuseTransmissionColorFactor[2]);
            assignSlotOpt(ext.DiffuseTransmissionTexture, static_cast<int>(MaterialTextureSlot::DiffuseTransmission),
                          dt.diffuseTransmissionTexture, RHI::SamplerDesc::ColorSpace::Linear);
            assignSlotOpt(ext.DiffuseTransmissionColorTexture,
                          static_cast<int>(MaterialTextureSlot::DiffuseTransmissionColor),
                          dt.diffuseTransmissionColorTexture);
        }

        // How to draw a transmission surface. Chosen once here rather than
        // per-draw in the renderer, so the decision is in one place and the
        // renderer only has to switch on it.
        if (ext.HasTransmission && ext.TransmissionFactor > 0.0f)
        {
            // A transmissive surface is see-through by definition, so it needs
            // the blended path even when the file did not set alphaMode BLEND -
            // which is the common case for glass exported from DCC tools.
            ext.TransmissionBlend = BlendApproach::Blend;
            m_HasTransparentMaterials = true;
        }
        else if (ext.HasTransmission)
        {
            ext.TransmissionBlend = BlendApproach::OpaqueWithTransmission;
        }
    }

    if (m_HasTransparentMaterials)
    {
        UHE_CORE_WARN("glTF uses alphaMode BLEND - transparent materials draw in submission order until a "
                      "sorted transparent pass exists (roadmap M3 step 4)");
    }
}

void Model::ParsePunctualLights(const fastgltf::Asset& asset)
{
    m_PunctualLights.clear();
    m_PunctualLights.reserve(asset.lights.size());

    for (const auto& light : asset.lights)
    {
        PunctualLight out;
        switch (light.type)
        {
            case fastgltf::LightType::Directional:
                out.type = PunctualLight::Type::Directional;
                break;
            case fastgltf::LightType::Spot:
                out.type = PunctualLight::Type::Spot;
                break;
            case fastgltf::LightType::Point:
            default:
                out.type = PunctualLight::Type::Point;
                break;
        }

        out.Color = glm::vec3(light.color.x(), light.color.y(), light.color.z());
        out.Intensity = static_cast<float>(light.intensity);
        out.Range = light.range.has_value() ? static_cast<float>(*light.range) : std::numeric_limits<float>::infinity();
        out.InnerConeAngle = static_cast<float>(light.innerConeAngle.value_or(0.0));
        out.OuterConeAngle = static_cast<float>(light.outerConeAngle.value_or(glm::pi<double>() / 4.0));
        out.Name = std::string(light.name);

        m_PunctualLights.push_back(std::move(out));
    }

    if (!m_PunctualLights.empty())
        UHE_CORE_INFO("glTF carries {0} punctual light(s) in asset.extensions", m_PunctualLights.size());
}

void Model::ProcessNode(const fastgltf::Asset& asset, size_t nodeIndex, const glm::mat4& parentTransform)
{
    // Issue #17: record the full glTF node tree (flattened) so each node can
    // later become an editable child entity in the scene hierarchy.
    const int thisIndex = static_cast<int>(m_Nodes.size());
    ModelNode& out = m_Nodes.emplace_back();

    const auto& node = asset.nodes[nodeIndex];
    out.Name = node.name.empty() ? "Node_" + std::to_string(nodeIndex) : std::string(node.name);

    // Local transform (glTF: TRS or matrix)
    std::visit(fastgltf::visitor{[&](const fastgltf::math::fmat4x4& matrix)
                                 {
                                     glm::mat4 m{1.0f};
                                     memcpy(&m, matrix.data(), sizeof(glm::mat4));
                                     glm::vec3 translation, scale;
                                     glm::quat rotation;
                                     glm::vec3 skew;
                                     glm::vec4 perspective;
                                     if (glm::decompose(m, scale, rotation, translation, skew, perspective))
                                     {
                                         out.Translation = translation;
                                         out.Rotation = rotation;
                                         out.Scale = scale;
                                     }
                                 },
                                 [&](const fastgltf::TRS& trs)
                                 {
                                     out.Translation =
                                         glm::vec3(trs.translation[0], trs.translation[1], trs.translation[2]);
                                     out.Rotation = glm::quat(trs.rotation[3], trs.rotation[0], trs.rotation[1],
                                                              trs.rotation[2]); // w,x,y,z
                                     out.Scale = glm::vec3(trs.scale[0], trs.scale[1], trs.scale[2]);
                                 }},
               node.transform);

    // The transform contributed by every ancestor. Without accumulating this,
    // every mesh in the file collapses onto the origin - the defect that made
    // multi-node assets (foliage especially) render wrong.
    const glm::mat4 worldTransform = parentTransform * NodeLocalTransform(node);

    // KHR_lights_punctual: which of the asset's lights this node places. The
    // index is validated here so the scene code that instantiates the light
    // cannot read past the light list on a malformed file.
    if (node.lightIndex.has_value())
    {
        if (*node.lightIndex < m_PunctualLights.size())
            out.LightIndex = static_cast<int>(*node.lightIndex);
        else
            UHE_CORE_WARN("Node {0} references light {1} but the file declares {2} lights; ignoring", nodeIndex,
                          *node.lightIndex, m_PunctualLights.size());
    }

    // Authoritative accumulated placement for node-carried lights. Written
    // AFTER children recurse can't touch it - `out` would dangle (see the note
    // at the bottom of this function), so set it before recursing.
    out.WorldTransform = worldTransform;

    if (node.meshIndex.has_value())
    {
        out.MeshIndex = static_cast<int>(m_LoadedMeshes.size());
        m_NodeToMesh[static_cast<int>(nodeIndex)] = out.MeshIndex;

        const size_t gltfMeshIndex = *node.meshIndex;

        // Extract a given glTF mesh at most once, however many nodes use it.
        auto [it, inserted] = m_GeometryCache.emplace(gltfMeshIndex, m_Geometry.size());
        if (inserted)
        {
            ExtractGeometry(asset, gltfMeshIndex, m_Options);
            it->second = m_Geometry.size() - 1;
        }
        const size_t geometryIndex = it->second;
        const Geometry& geom = m_Geometry[geometryIndex];

        Mesh mesh;
        mesh.geometryIndex = geometryIndex;
        mesh.nodeIndex = nodeIndex;
        mesh.LocalTransform = worldTransform;

        // Node name is what the hierarchy panel should show. The glTF mesh name is
        // only a fallback, because every instance of one mesh shares it.
        const std::string nodeName(node.name);
        mesh.name = !nodeName.empty() ? nodeName : geom.name;

        mesh.hasBounds = geom.hasBounds;
        if (geom.hasBounds)
        {
            // Transform all 8 AABB corners: a rotated node's world bounds are not
            // a transform of the local bounds, so min/max must be recomputed.
            glm::vec3 worldMin(std::numeric_limits<f32>::max());
            glm::vec3 worldMax(-std::numeric_limits<f32>::max());
            for (int corner = 0; corner < 8; ++corner)
            {
                const glm::vec3 local((corner & 1) ? geom.boundsMax.x : geom.boundsMin.x,
                                      (corner & 2) ? geom.boundsMax.y : geom.boundsMin.y,
                                      (corner & 4) ? geom.boundsMax.z : geom.boundsMin.z);
                // glm is column-major; vec4 * mat4 applies the transform.
                const glm::vec3 world = glm::vec3(glm::vec4(local, 1.0f) * worldTransform);
                worldMin = glm::min(worldMin, world);
                worldMax = glm::max(worldMax, world);
            }
            mesh.boundsMin = worldMin;
            mesh.boundsMax = worldMax;
        }

        for (const auto& srcPrim : geom.primitive)
        {
            Primitive prim;
            prim.materialIndex = srcPrim.materialIndex;
            prim.geometryIndex = geometryIndex;
            // Borrow the handles; the geometry owns them and frees them once.
            prim.usesSharedGeometry = true;
            prim.VertexBuffer = srcPrim.VertexBuffer;
            prim.IndexBuffer = srcPrim.IndexBuffer;
            prim.IndexCount = srcPrim.IndexCount;
            mesh.primitive.push_back(prim);
        }

        m_LoadedMeshes.push_back(std::move(mesh));
    }

    // NOTE: recurse first, then write through indices — `out` would dangle
    // once ProcessNode appends more nodes to m_Nodes.
    for (auto& childIndex : node.children)
    {
        const int childSlot = static_cast<int>(m_Nodes.size());
        ProcessNode(asset, childIndex, worldTransform);
        m_Nodes[childSlot].Parent = thisIndex;
        m_Nodes[thisIndex].Children.push_back(childSlot);
    }
}
void Model::ComputeBounds(Geometry& geometry)
{
    glm::vec3 min(std::numeric_limits<f32>::max());
    glm::vec3 max(-std::numeric_limits<f32>::max());
    bool any = false;

    for (const auto& prim : geometry.primitive)
    {
        for (const auto& vertex : prim.vertices)
        {
            min = glm::min(min, vertex.position);
            max = glm::max(max, vertex.position);
            any = true;
        }
    }

    if (any)
    {
        geometry.boundsMin = min;
        geometry.boundsMax = max;
        geometry.hasBounds = true;
    }
}

} // namespace UHE::RD3d
