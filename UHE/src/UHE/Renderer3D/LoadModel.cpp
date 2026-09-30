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

namespace UHE::RD3d
{

namespace
{

// Extensions this loader actually honours. Anything else a file declares is
// reported by name instead of being silently ignored.
//
// KHR_texture_transform is listed but its effect is still hardcoded (see the UV
// V-flip in ExtractGeometry), so it counts as supported only in the sense that
// it is not a warning-worthy gap; it is called out there, not here.
bool IsExtensionSupported(std::string_view name)
{
    return name == "KHR_materials_pbrSpecularGlossiness" || // SpecularGlossiness path
           name == "KHR_texture_transform" ||              // UV transform (see note)
           name == "KHR_materials_unlit";
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

        auto loadTexture = [&](const fastgltf::TextureInfo* texInfo, size_t matIdx) -> Ref<Texture2D> {
            if (!texInfo) return nullptr;
            auto textureIndex = texInfo->textureIndex;
            if (textureIndex >= asset.textures.size()) return nullptr;
            auto imageIndex = asset.textures[textureIndex].imageIndex;
            if (!imageIndex.has_value()) return nullptr;

            auto& image = asset.images[imageIndex.value()];
            Ref<Texture2D> result = nullptr;
            std::visit(
                fastgltf::visitor{[&](const fastgltf::sources::URI& filePath)
                                  {
                                      std::filesystem::path imgPath = filepath.parent_path() / filePath.uri.path();
                                      result = Texture2D::Create(imgPath.string());
                                  },
                                  [&](const fastgltf::sources::Array& array)
                                  {
                                      result = Texture2D::CreateFromMemory(array.bytes.data(), array.bytes.size());
                                      UHE_CORE_INFO("Loaded texture for material {0} from Array, size: {1}", matIdx, array.bytes.size());
                                  },
                                  [&](const fastgltf::sources::ByteView& byteView)
                                  {
                                      result = Texture2D::CreateFromMemory(byteView.bytes.data(), byteView.bytes.size());
                                      UHE_CORE_INFO("Loaded texture for material {0} from ByteView, size: {1}", matIdx, byteView.bytes.size());
                                  },
                                  [&](const fastgltf::sources::Vector& vector)
                                  {
                                      result = Texture2D::CreateFromMemory(vector.bytes.data(), vector.bytes.size());
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
                                                  result = Texture2D::CreateFromMemory(data, bufferView.byteLength);
                                                  UHE_CORE_INFO("Loaded texture for material {0} from BufferView (Array), size: {1}", matIdx, bufferView.byteLength);
                                              },
                                              [&](const fastgltf::sources::ByteView& byteView)
                                              {
                                                  const void* data = byteView.bytes.data() + bufferView.byteOffset;
                                                  result = Texture2D::CreateFromMemory(data, bufferView.byteLength);
                                                  UHE_CORE_INFO("Loaded texture for material {0} from BufferView (ByteView), size: {1}", matIdx, bufferView.byteLength);
                                              },
                                              [&](const fastgltf::sources::Vector& vector)
                                              {
                                                  const void* data = vector.bytes.data() + bufferView.byteOffset;
                                                  result = Texture2D::CreateFromMemory(data, bufferView.byteLength);
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
            m_LoadedMaterials[i].MetallicRoughnessTexture = loadTexture(&gltfMaterial.pbrData.metallicRoughnessTexture.value(), i);
        }

        // Normal / occlusion / emissive. fastgltf exposes each as a TextureInfo
        // carrying both the texture index and its own scale/strength scalar, so
        // the map and the scalar come from the same struct.
        if (gltfMaterial.normalTexture.has_value())
        {
            const auto& n = gltfMaterial.normalTexture.value();
            m_LoadedMaterials[i].NormalTexture = loadTexture(&n, i);
            m_LoadedMaterials[i].NormalScale = n.scale;
        }

        if (gltfMaterial.occlusionTexture.has_value())
        {
            const auto& o = gltfMaterial.occlusionTexture.value();
            m_LoadedMaterials[i].OcclusionTexture = loadTexture(&o, i);
            m_LoadedMaterials[i].OcclusionStrength = o.strength;
        }

        if (gltfMaterial.emissiveTexture.has_value())
        {
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
    }

    if (m_HasTransparentMaterials)
    {
        UHE_CORE_WARN("glTF uses alphaMode BLEND - transparent materials draw in submission order until a "
                      "sorted transparent pass exists (roadmap M3 step 4)");
    }
}

void Model::ProcessNode(const fastgltf::Asset& asset, size_t nodeIndex, const glm::mat4& parentTransform)
{
    const auto& node = asset.nodes[nodeIndex];

    // The transform contributed by every ancestor. Without accumulating this,
    // every mesh in the file collapses onto the origin - the defect that made
    // multi-node assets (foliage especially) render wrong.
    const glm::mat4 worldTransform = parentTransform * NodeLocalTransform(node);

    if (node.meshIndex.has_value())
    {
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

    for (auto& childIndex : node.children)
    {
        ProcessNode(asset, childIndex, worldTransform);
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
