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
#include "UHE/RHI/RHICommadBuffer.h"
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
           name == "KHR_texture_transform" ||              // UV transform (see note)
           name == "KHR_materials_unlit" ||                 // bypasses shading
           name == "KHR_materials_emissive_strength" ||     // emissive multiplier
           name == "KHR_materials_ior" ||                   // dielectric F0
           name == "KHR_materials_clearcoat" ||             // second specular lobe
           name == "KHR_materials_specular" ||              // custom F0 + colour
           name == "KHR_materials_sheen" ||                 // fabric retroreflection
           name == "KHR_materials_transmission" ||          // see-through
           name == "KHR_materials_volume" ||                // medium attenuation
           name == "KHR_materials_iridescence" ||           // thin-film
           name == "KHR_materials_anisotropy";              // brushed metal
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
}

glm::mat4 Model::NodeLocalTransform(const fastgltf::Node& node)
{
    glm::mat4 local{1.0f};

    std::visit(fastgltf::visitor{
                   [&](const fastgltf::math::fmat4x4& matrix) {
                       std::memcpy(&local, matrix.data(), sizeof(glm::mat4));
                   },
                   [&](const fastgltf::TRS& trs) {
                       const glm::vec3 T(trs.translation[0], trs.translation[1], trs.translation[2]);
                       // glTF stores quaternions as x, y, z, w; glm::quat takes w, x, y, z.
                       const glm::quat R(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
                       const glm::vec3 S(trs.scale[0], trs.scale[1], trs.scale[2]);
                       local = glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) * glm::scale(glm::mat4(1.0f), S);
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
    const auto toFilter = [](fastgltf::Filter f) {
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

} // namespace

void Model::LoadMaterials(const fastgltf::Asset& asset, const std::filesystem::path& filepath)
{
    m_LoadedMaterials.resize(asset.materials.size());

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
                               RHI::SamplerDesc::ColorSpace colorSpace = RHI::SamplerDesc::ColorSpace::SRGB) -> Ref<Texture2D> {
            if (!texInfo) return nullptr;
            auto textureIndex = texInfo->textureIndex;
            if (textureIndex >= asset.textures.size()) return nullptr;
            auto imageIndex = asset.textures[textureIndex].imageIndex;
            if (!imageIndex.has_value()) return nullptr;

            auto& image = asset.images[imageIndex.value()];

            // glTF declares sampler state per texture (magFilter/minFilter/wrapS/
            // wrapT). Without this the backend's hardcoded linear/repeat is used
            // for everything, so a CLAMP_TO_EDGE atlas or a NEAREST pixel-art
            // texture samples wrongly.
            RHI::SamplerDesc sampler = SamplerDescForTexture(asset, textureIndex, colorSpace);

            Ref<Texture2D> result = nullptr;
            std::visit(
                fastgltf::visitor{[&](const fastgltf::sources::URI& filePath)
                                  {
                                      std::filesystem::path imgPath = filepath.parent_path() / filePath.uri.path();
                                      result = Texture2D::Create(imgPath.string(), sampler);
                                  },
                                  [&](const fastgltf::sources::Array& array)
                                  {
                                      result = Texture2D::CreateFromMemory(array.bytes.data(), array.bytes.size(), sampler);
                                      UHE_CORE_INFO("Loaded texture for material {0} from Array, size: {1}", matIdx, array.bytes.size());
                                  },
                                  [&](const fastgltf::sources::ByteView& byteView)
                                  {
                                      result = Texture2D::CreateFromMemory(byteView.bytes.data(), byteView.bytes.size(), sampler);
                                      UHE_CORE_INFO("Loaded texture for material {0} from ByteView, size: {1}", matIdx, byteView.bytes.size());
                                  },
                                  [&](const fastgltf::sources::Vector& vector)
                                  {
                                      result = Texture2D::CreateFromMemory(vector.bytes.data(), vector.bytes.size(), sampler);
                                      UHE_CORE_INFO("Loaded texture for material {0} from Vector, size: {1}", matIdx, vector.bytes.size());
                                  },
                                  [&](const fastgltf::sources::Fallback& fallback)
                                  {
                                      UHE_CORE_ERROR("fastgltf fallback triggered for material {0} image! Image could not be loaded.", matIdx);
                                  },
                                  [&](const fastgltf::sources::BufferView& view)
                                  {
                                      if (view.bufferViewIndex >= asset.bufferViews.size() ||
                                          asset.bufferViews[view.bufferViewIndex].bufferIndex >= asset.buffers.size())
                                      {
                                          UHE_CORE_ERROR("BufferView for material {0} is out of range", matIdx);
                                          return;
                                      }
                                      auto& bufferView = asset.bufferViews[view.bufferViewIndex];
                                      auto& buffer = asset.buffers[bufferView.bufferIndex];
                                      std::visit(
                                          fastgltf::visitor{
                                              [&](const fastgltf::sources::Array& array)
                                              {
                                                  const void* data = array.bytes.data() + bufferView.byteOffset;
                                                  result = Texture2D::CreateFromMemory(data, bufferView.byteLength, sampler);
                                                  UHE_CORE_INFO("Loaded texture for material {0} from BufferView (Array), size: {1}", matIdx, bufferView.byteLength);
                                              },
                                              [&](const fastgltf::sources::ByteView& byteView)
                                              {
                                                  const void* data = byteView.bytes.data() + bufferView.byteOffset;
                                                  result = Texture2D::CreateFromMemory(data, bufferView.byteLength, sampler);
                                                  UHE_CORE_INFO("Loaded texture for material {0} from BufferView (ByteView), size: {1}", matIdx, bufferView.byteLength);
                                              },
                                              [&](const fastgltf::sources::Vector& vector)
                                              {
                                                  const void* data = vector.bytes.data() + bufferView.byteOffset;
                                                  result = Texture2D::CreateFromMemory(data, bufferView.byteLength, sampler);
                                                  UHE_CORE_INFO("Loaded texture for material {0} from BufferView (Vector), size: {1}", matIdx, bufferView.byteLength);
                                              },
                                              [&](const auto&) {
                                                  UHE_CORE_ERROR("Unhandled buffer data type in glTF! Variant index: {0}", buffer.data.index());
                                              }},
                                          buffer.data);
                                  },
                                  [&](const auto&) {
                                      UHE_CORE_ERROR("Unhandled image data type in glTF! Variant index: {0}", image.data.index());
                                  }},
                image.data);
            return result;
        };

        if (albedoTextureInfo)
        {
            m_LoadedMaterials[i].AlbedoTexture = loadTexture(albedoTextureInfo, i);
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
            m_LoadedMaterials[i].MetallicRoughnessTexture = loadTexture(
                &gltfMaterial.pbrData.metallicRoughnessTexture.value(), i, RHI::SamplerDesc::ColorSpace::Linear);
        }

        // Normal / occlusion / emissive. fastgltf exposes each as a TextureInfo
        // carrying both the texture index and its own scale/strength scalar, so
        // the map and the scalar come from the same struct.
        if (gltfMaterial.normalTexture.has_value())
        {
            const auto& n = gltfMaterial.normalTexture.value();
            m_LoadedMaterials[i].NormalTexture = loadTexture(&n, i, RHI::SamplerDesc::ColorSpace::Linear);
            m_LoadedMaterials[i].NormalScale = n.scale;
        }

        if (gltfMaterial.occlusionTexture.has_value())
        {
            const auto& o = gltfMaterial.occlusionTexture.value();
            // Occlusion is a scalar cavity term in the R channel - LINEAR.
            m_LoadedMaterials[i].OcclusionTexture = loadTexture(&o, i, RHI::SamplerDesc::ColorSpace::Linear);
            m_LoadedMaterials[i].OcclusionStrength = o.strength;
        }

        if (gltfMaterial.emissiveTexture.has_value())
        {
            // Emissive is colour, so sRGB - the default the lambda already applies.
            m_LoadedMaterials[i].EmissiveTexture = loadTexture(&gltfMaterial.emissiveTexture.value(), i);
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

        if (gltfMaterial.clearcoat)
        {
            const auto& cc = *gltfMaterial.clearcoat;
            ext.HasClearcoat = true;
            ext.ClearcoatFactor = cc.clearcoatFactor;
            ext.ClearcoatRoughnessFactor = cc.clearcoatRoughnessFactor;
            if (cc.clearcoatNormalTexture)
                ext.ClearcoatNormalScale = cc.clearcoatNormalTexture->scale;
            // The clearcoat maps are loaded so their textures exist and get a
            // descriptor slot, but the shader reads them from the per-material
            // buffer rather than a push-constant int - the push-constant block
            // has no room left.
            if (cc.clearcoatTexture)
                ext.ClearcoatTexture = loadTexture(&*cc.clearcoatTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
            if (cc.clearcoatRoughnessTexture)
                ext.ClearcoatRoughnessTexture =
                    loadTexture(&*cc.clearcoatRoughnessTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
            if (cc.clearcoatNormalTexture)
                ext.ClearcoatNormalTexture =
                    loadTexture(&*cc.clearcoatNormalTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
        }

        if (gltfMaterial.specular)
        {
            const auto& sp = *gltfMaterial.specular;
            ext.HasSpecular = true;
            ext.SpecularFactor = sp.specularFactor;
            ext.SpecularColorFactor = glm::vec3(sp.specularColorFactor[0], sp.specularColorFactor[1],
                                                 sp.specularColorFactor[2]);
            if (sp.specularTexture)
                ext.SpecularTexture = loadTexture(&*sp.specularTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
            if (sp.specularColorTexture)
                ext.SpecularColorTexture = loadTexture(&*sp.specularColorTexture, i);
        }

        if (gltfMaterial.sheen)
        {
            const auto& sh = *gltfMaterial.sheen;
            ext.HasSheen = true;
            ext.SheenColorFactor = glm::vec3(sh.sheenColorFactor[0], sh.sheenColorFactor[1], sh.sheenColorFactor[2]);
            ext.SheenRoughnessFactor = sh.sheenRoughnessFactor;
            if (sh.sheenColorTexture)
                ext.SheenColorTexture = loadTexture(&*sh.sheenColorTexture, i);
            if (sh.sheenRoughnessTexture)
                ext.SheenRoughnessTexture = loadTexture(&*sh.sheenRoughnessTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
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
                if (tr.transmissionTexture)
                    ext.TransmissionTexture =
                        loadTexture(&*tr.transmissionTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
            }
            if (gltfMaterial.volume)
            {
                const auto& vo = *gltfMaterial.volume;
                ext.ThicknessFactor = vo.thicknessFactor;
                ext.AttenuationDistance = vo.attenuationDistance;
                ext.AttenuationColor =
                    glm::vec3(vo.attenuationColor[0], vo.attenuationColor[1], vo.attenuationColor[2]);
                if (vo.thicknessTexture)
                    ext.ThicknessTexture = loadTexture(&*vo.thicknessTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
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
            if (ir.iridescenceTexture)
                ext.IridescenceTexture = loadTexture(&*ir.iridescenceTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
            if (ir.iridescenceThicknessTexture)
                ext.IridescenceThicknessTexture =
                    loadTexture(&*ir.iridescenceThicknessTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
        }

        if (gltfMaterial.anisotropy)
        {
            const auto& an = *gltfMaterial.anisotropy;
            ext.HasAnisotropy = true;
            ext.AnisotropyStrength = an.anisotropyStrength;
            ext.AnisotropyRotation = an.anisotropyRotation;
            if (an.anisotropyTexture)
                ext.AnisotropyTexture = loadTexture(&*an.anisotropyTexture, i, RHI::SamplerDesc::ColorSpace::Linear);
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

void Model::ProcessNode(const fastgltf::Asset& asset, size_t nodeIndex, const glm::mat4& parentTransform)
{
    // Issue #17: record the full glTF node tree (flattened) so each node can
    // later become an editable child entity in the scene hierarchy.
    const int thisIndex = static_cast<int>(m_Nodes.size());
    ModelNode& out = m_Nodes.emplace_back();

    const auto& node = asset.nodes[nodeIndex];
    out.Name = node.name.empty() ? "Node_" + std::to_string(nodeIndex) : std::string(node.name);

    // Local transform (glTF: TRS or matrix)
    std::visit(fastgltf::visitor{
                   [&](const fastgltf::math::fmat4x4& matrix)
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
                       out.Translation = glm::vec3(trs.translation[0], trs.translation[1], trs.translation[2]);
                       out.Rotation =
                           glm::quat(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]); // w,x,y,z
                       out.Scale = glm::vec3(trs.scale[0], trs.scale[1], trs.scale[2]);
                   }},
               node.transform);

    // The transform contributed by every ancestor. Without accumulating this,
    // every mesh in the file collapses onto the origin - the defect that made
    // multi-node assets (foliage especially) render wrong.
    const glm::mat4 worldTransform = parentTransform * NodeLocalTransform(node);

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
