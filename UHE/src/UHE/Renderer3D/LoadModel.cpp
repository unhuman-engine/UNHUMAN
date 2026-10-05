#include "uhepch.h"
#include "LoadModel.h"
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
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

Model::~Model()
{
    Destroy();
}

void Model::Destroy()
{
    auto& device = Renderer::GetDevice();

    m_Animations.clear();
    m_Skeleton.Bones.clear();
    m_Skeleton.JointNodes.clear();
    m_Skeleton.RootBoneID = -1;
    m_Skins.clear();
    m_Nodes.clear();
    m_RootNodes.clear();
    m_NodeToMesh.clear();

    for (auto& mesh : m_LoadedMeshes)
    {
        for (auto& prim : mesh.primitive)
        {
            if (prim.VertexBuffer)
            {
                device.DestroyBuffer(prim.VertexBuffer);
                prim.VertexBuffer = nullptr;
            }
            if (prim.IndexBuffer)
            {
                device.DestroyBuffer(prim.IndexBuffer);
                prim.IndexBuffer = nullptr;
            }
        }
    }
}

bool Model::loadModel(const std::filesystem::path& filepath)
{
    if (!std::filesystem::exists(filepath))
    {
        UHE_CORE_ERROR("File not Found {0}", filepath.string());
        return false;
    }

    Destroy();
    m_LoadedMeshes.clear();
    m_LoadedMaterials.clear();
    m_Animations.clear();
    m_Skeleton.Bones.clear();
    m_Skeleton.JointNodes.clear();
    m_Skeleton.RootBoneID = -1;
    m_Skins.clear();
    m_Nodes.clear();
    m_RootNodes.clear();
    m_NodeToMesh.clear();

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

        auto loadTexture = [&](const fastgltf::TextureInfo* texInfo, size_t matIdx) -> Ref<Texture2D>
        {
            if (!texInfo)
                return nullptr;
            auto textureIndex = texInfo->textureIndex;
            auto imageIndex = asset.textures[textureIndex].imageIndex;
            if (!imageIndex.has_value())
                return nullptr;

            auto& image = asset.images[imageIndex.value()];
            Ref<Texture2D> result = nullptr;
            std::visit(
                fastgltf::visitor{
                    [&](const fastgltf::sources::URI& filePath)
                    {
                        std::filesystem::path imgPath = filepath.parent_path() / filePath.uri.path();
                        result = Texture2D::Create(imgPath.string());
                    },
                    [&](const fastgltf::sources::Array& array)
                    {
                        result = Texture2D::CreateFromMemory(array.bytes.data(), array.bytes.size());
                        UHE_CORE_INFO("Loaded texture for material {0} from Array, size: {1}", matIdx,
                                      array.bytes.size());
                    },
                    [&](const fastgltf::sources::ByteView& byteView)
                    {
                        result = Texture2D::CreateFromMemory(byteView.bytes.data(), byteView.bytes.size());
                        UHE_CORE_INFO("Loaded texture for material {0} from ByteView, size: {1}", matIdx,
                                      byteView.bytes.size());
                    },
                    [&](const fastgltf::sources::Vector& vector)
                    {
                        result = Texture2D::CreateFromMemory(vector.bytes.data(), vector.bytes.size());
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
                        auto& bufferView = asset.bufferViews[view.bufferViewIndex];
                        auto& buffer = asset.buffers[bufferView.bufferIndex];
                        std::visit(
                            fastgltf::visitor{
                                [&](const fastgltf::sources::Array& array)
                                {
                                    const void* data = array.bytes.data() + bufferView.byteOffset;
                                    result = Texture2D::CreateFromMemory(data, bufferView.byteLength);
                                    UHE_CORE_INFO("Loaded texture for material {0} from BufferView (Array), size: {1}",
                                                  matIdx, bufferView.byteLength);
                                },
                                [&](const fastgltf::sources::ByteView& byteView)
                                {
                                    const void* data = byteView.bytes.data() + bufferView.byteOffset;
                                    result = Texture2D::CreateFromMemory(data, bufferView.byteLength);
                                    UHE_CORE_INFO(
                                        "Loaded texture for material {0} from BufferView (ByteView), size: {1}", matIdx,
                                        bufferView.byteLength);
                                },
                                [&](const fastgltf::sources::Vector& vector)
                                {
                                    const void* data = vector.bytes.data() + bufferView.byteOffset;
                                    result = Texture2D::CreateFromMemory(data, bufferView.byteLength);
                                    UHE_CORE_INFO("Loaded texture for material {0} from BufferView (Vector), size: {1}",
                                                  matIdx, bufferView.byteLength);
                                },
                                [&](const auto&)
                                {
                                    UHE_CORE_ERROR("Unhandled buffer data type in glTF! Variant index: {0}",
                                                   buffer.data.index());
                                }},
                            buffer.data);
                    },
                    [&](const auto&)
                    { UHE_CORE_ERROR("Unhandled image data type in glTF! Variant index: {0}", image.data.index()); }},
                image.data);
            return result;
        };

        if (albedoTextureInfo)
        {
            m_LoadedMaterials[i].AlbedoTexture = loadTexture(albedoTextureInfo, i);
        }

        m_LoadedMaterials[i].MetallicFactor = gltfMaterial.pbrData.metallicFactor;
        m_LoadedMaterials[i].RoughnessFactor = gltfMaterial.pbrData.roughnessFactor;

        if (gltfMaterial.pbrData.metallicRoughnessTexture.has_value())
        {
            m_LoadedMaterials[i].MetallicRoughnessTexture =
                loadTexture(&gltfMaterial.pbrData.metallicRoughnessTexture.value(), i);
        }
    }

    ParseSkins(asset);
    ParseAnimations(asset);

    size_t activeSceneIndex = asset.defaultScene.value_or(0);
    if (!asset.scenes.empty() && activeSceneIndex < asset.scenes.size())
    {
        auto& scene = asset.scenes[activeSceneIndex];
        for (auto& rootNodeIndex : scene.nodeIndices)
        {
            m_RootNodes.push_back(static_cast<int>(m_Nodes.size()));
            ProcessNode(asset, rootNodeIndex);
        }
    }
    return true;
}

void Model::ProcessNode(const fastgltf::Asset& asset, size_t nodeIndex)
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

    if (node.meshIndex.has_value())
    {
        out.MeshIndex = static_cast<int>(m_LoadedMeshes.size());
        m_NodeToMesh[static_cast<int>(nodeIndex)] = out.MeshIndex;
        ExtractMesh(asset, asset.meshes[node.meshIndex.value()]);
    }

    // NOTE: recurse first, then write through indices — `out` would dangle
    // once ProcessNode appends more nodes to m_Nodes.
    for (auto& childIndex : node.children)
    {
        const int childSlot = static_cast<int>(m_Nodes.size());
        ProcessNode(asset, childIndex);
        m_Nodes[childSlot].Parent = thisIndex;
        m_Nodes[thisIndex].Children.push_back(childSlot);
    }
}

void Model::ExtractMesh(const fastgltf::Asset& asset, const fastgltf::Mesh& gltfMesh)
{
    Mesh outMesh;
    outMesh.name = gltfMesh.name.empty() ? "Unnamed_Mesh" : std::string(gltfMesh.name);

    for (auto& primitive : gltfMesh.primitives)
    {
        Primitive outPrim;
        outPrim.materialIndex = primitive.materialIndex.value_or(0);
        const auto* posAttribute = primitive.findAttribute("POSITION");
        if (posAttribute == primitive.attributes.end())
            continue;

        auto& posAccessor = asset.accessors[posAttribute->accessorIndex];
        outPrim.vertices.resize(posAccessor.count);

        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, posAccessor,
                                                                  [&](fastgltf::math::fvec3 pos, size_t idx)
                                                                  {
                                                                      outPrim.vertices[idx].position =
                                                                          glm::vec3(pos.x(), pos.y(), pos.z());
                                                                      outPrim.vertices[idx].normal =
                                                                          glm::vec3(0.0f, 1.0f, 0.0f); // Default
                                                                      outPrim.vertices[idx].uv = glm::vec2(0.0f);
                                                                  });

        const auto* normalAttribute = primitive.findAttribute("NORMAL");
        if (normalAttribute != primitive.attributes.end())
        {
            auto& normalAccessor = asset.accessors[normalAttribute->accessorIndex];
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                asset, normalAccessor, [&](fastgltf::math::fvec3 norm, size_t idx)
                { outPrim.vertices[idx].normal = glm::vec3(norm.x(), norm.y(), norm.z()); });
        }

        const auto* uvAttribute = primitive.findAttribute("TEXCOORD_0");
        if (uvAttribute != primitive.attributes.end())
        {
            auto& uvAccessor = asset.accessors[uvAttribute->accessorIndex];

            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(
                asset, uvAccessor, [&](fastgltf::math::fvec2 uv, size_t idx)
                { outPrim.vertices[idx].uv = glm::vec2(uv.x(), 1.0f - uv.y()); });
        }

        const auto* jointsAttribute = primitive.findAttribute("JOINTS_0");
        if (jointsAttribute != primitive.attributes.end())
        {
            auto& jointsAccessor = asset.accessors[jointsAttribute->accessorIndex];
            fastgltf::iterateAccessorWithIndex<fastgltf::math::uvec4>(
                asset, jointsAccessor, [&](fastgltf::math::uvec4 joints, size_t idx)
                { outPrim.vertices[idx].jointIndices = glm::ivec4(joints.x(), joints.y(), joints.z(), joints.w()); });
        }

        const auto* weightsAttribute = primitive.findAttribute("WEIGHTS_0");
        if (weightsAttribute != primitive.attributes.end())
        {
            auto& weightsAccessor = asset.accessors[weightsAttribute->accessorIndex];
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                asset, weightsAccessor,
                [&](fastgltf::math::fvec4 weights, size_t idx)
                {
                    outPrim.vertices[idx].jointWeights = glm::vec4(weights.x(), weights.y(), weights.z(), weights.w());
                });
        }

        if (primitive.indicesAccessor.has_value())
        {
            auto& indicesAccessor = asset.accessors[primitive.indicesAccessor.value()];
            outPrim.indices.reserve(indicesAccessor.count);

            fastgltf::iterateAccessor<u32>(asset, indicesAccessor,
                                           [&](u32 indexValue) { outPrim.indices.push_back(indexValue); });
        }

        auto& device = Renderer::GetDevice();
        auto& cmd = device.GetCurrentCommandBuffer();

        // 1. Create and Upload Vertex Buffer
        if (!outPrim.vertices.empty())
        {
            RHI::BufferDesc vbDesc{};
            vbDesc.size = outPrim.vertices.size() * sizeof(Vertex);
            vbDesc.usage = RHI::BufferUsage::Vertex;
            vbDesc.hostVisible = true;
            outPrim.VertexBuffer = device.CreateBuffer(vbDesc);
            cmd.UpdateBuffer(outPrim.VertexBuffer, outPrim.vertices.data(), vbDesc.size);
        }

        // 2. Create and Upload Index Buffer
        if (!outPrim.indices.empty())
        {
            outPrim.IndexCount = static_cast<uint32_t>(outPrim.indices.size());
            RHI::BufferDesc ibDesc{};
            ibDesc.size = outPrim.indices.size() * sizeof(u32);
            ibDesc.usage = RHI::BufferUsage::Index;
            ibDesc.hostVisible = true;
            outPrim.IndexBuffer = device.CreateBuffer(ibDesc);
            cmd.UpdateBuffer(outPrim.IndexBuffer, outPrim.indices.data(), ibDesc.size);
        }

        outMesh.primitive.push_back(std::move(outPrim));
    }
    m_LoadedMeshes.push_back(std::move(outMesh));
}

void Model::ParseSkins(const fastgltf::Asset& asset)
{
    if (asset.skins.empty())
        return;

    // The bone hierarchy covers every node once (bones == glTF nodes); each
    // skin then selects its own joint set and inverse bind matrices.
    // Issue #41: all skins are parsed now, not just the first one.
    m_Skeleton.Bones.resize(asset.nodes.size());
    m_Skeleton.RootBoneID = asset.skins[0].skeleton.has_value()
                                ? static_cast<int>(asset.skins[0].skeleton.value())
                                : (asset.skins[0].joints.empty() ? -1 : static_cast<int>(asset.skins[0].joints[0]));

    // 1. Build hierarchy mapping from asset.nodes
    for (size_t i = 0; i < asset.nodes.size(); ++i)
    {
        const auto& node = asset.nodes[i];
        m_Skeleton.Bones[i].ID = i;
        m_Skeleton.Bones[i].Name = node.name.empty() ? "Bone_" + std::to_string(i) : std::string(node.name);

        // Extract local transform
        glm::mat4 localTransform{1.0f};
        // Actually fastgltf provides a helper or we can parse it
        std::visit(fastgltf::visitor{[&](const fastgltf::math::fmat4x4& matrix)
                                     { memcpy(&localTransform, matrix.data(), sizeof(glm::mat4)); },
                                     [&](const fastgltf::TRS& trs)
                                     {
                                         glm::vec3 T(trs.translation[0], trs.translation[1], trs.translation[2]);
                                         glm::quat R(trs.rotation[3], trs.rotation[0], trs.rotation[1],
                                                     trs.rotation[2]); // w, x, y, z
                                         glm::vec3 S(trs.scale[0], trs.scale[1], trs.scale[2]);
                                         localTransform = glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) *
                                                          glm::scale(glm::mat4(1.0f), S);
                                     }},
                   node.transform);

        m_Skeleton.Bones[i].LocalTransform = localTransform;

        // Set parent
        for (auto childIdx : node.children)
        {
            m_Skeleton.Bones[childIdx].ParentID = i;
        }
    }

    // 2. Every skin: joint list + inverse bind matrices.
    for (size_t s = 0; s < asset.skins.size(); ++s)
    {
        const auto& gltfSkin = asset.skins[s];
        Skin skin;
        skin.Name = gltfSkin.name.empty() ? "Skin_" + std::to_string(s) : std::string(gltfSkin.name);
        skin.JointNodes.assign(gltfSkin.joints.begin(), gltfSkin.joints.end());
        skin.InverseBindMatrices.resize(gltfSkin.joints.size(), glm::mat4(1.0f));

        if (gltfSkin.inverseBindMatrices.has_value())
        {
            auto& ibmAccessor = asset.accessors[gltfSkin.inverseBindMatrices.value()];
            size_t jointIdx = 0;
            fastgltf::iterateAccessor<fastgltf::math::fmat4x4>(asset, ibmAccessor,
                                                               [&](const fastgltf::math::fmat4x4& matrix)
                                                               {
                                                                   if (jointIdx < skin.InverseBindMatrices.size())
                                                                   {
                                                                       memcpy(&skin.InverseBindMatrices[jointIdx],
                                                                              matrix.data(), sizeof(glm::mat4));
                                                                   }
                                                                   jointIdx++;
                                                               });
        }

        // The first skin also feeds the legacy Skeleton fields so existing
        // call sites (GetSkeleton) keep working unchanged.
        if (s == 0)
        {
            m_Skeleton.JointNodes = skin.JointNodes;
            for (size_t j = 0; j < skin.JointNodes.size(); ++j)
                m_Skeleton.Bones[skin.JointNodes[j]].InverseBindMatrix = skin.InverseBindMatrices[j];
        }

        m_Skins.push_back(std::move(skin));
    }
}

// Returns the track already targeting this bone, or appends a new one. glTF
// may emit several channels for the same (bone, path) pair; merging them
// keeps the sampled pose well-defined.
template <typename TrackT>
static TrackT* FindOrAddTrack(std::vector<TrackT>& tracks, int targetBone)
{
    for (auto& track : tracks)
        if (track.TargetBoneID == targetBone)
            return &track;
    TrackT& track = tracks.emplace_back();
    track.TargetBoneID = targetBone;
    return &track;
}

// CUBICSPLINE output layout per glTF 2.0: for key k the accessor stores
// [inTangent_k, value_k, outTangent_k]; tangents are scaled by the segment
// duration at evaluation time.
template <typename T, typename GltfT, typename ToGlm>
static void AppendTrackKeys(const fastgltf::Asset& asset, const fastgltf::Accessor& valueAccessor,
                            const std::vector<float>& times, InterpolationMode interpolation,
                            std::vector<Keyframe<T>>& keys, std::vector<T>& inTangents,
                            std::vector<T>& outTangents, ToGlm toGlm)
{
    const bool spline = interpolation == InterpolationMode::CubicSpline;
    if (spline)
    {
        keys.resize(times.size());
        inTangents.resize(times.size());
        outTangents.resize(times.size());
        fastgltf::iterateAccessorWithIndex<GltfT>(asset, valueAccessor,
                                                  [&](GltfT v, size_t i)
                                                  {
                                                      const size_t key = i / 3;
                                                      if (key >= times.size())
                                                          return;
                                                      const size_t part = i % 3; // 0 in, 1 value, 2 out
                                                      if (part == 0)
                                                          inTangents[key] = toGlm(v);
                                                      else if (part == 1)
                                                          keys[key] = {times[key], toGlm(v)};
                                                      else
                                                          outTangents[key] = toGlm(v);
                                                  });
    }
    else
    {
        size_t idx = 0;
        fastgltf::iterateAccessor<GltfT>(asset, valueAccessor,
                                         [&](GltfT v)
                                         {
                                             if (idx < times.size())
                                                 keys.push_back({times[idx], toGlm(v)});
                                             ++idx;
                                         });
    }
}

void Model::ParseAnimations(const fastgltf::Asset& asset)
{
    if (asset.animations.empty())
        return;

    for (const auto& gltfAnim : asset.animations)
    {
        AnimationClip clip;
        clip.Name = gltfAnim.name.empty() ? "Anim_" + std::to_string(m_Animations.size()) : std::string(gltfAnim.name);

        for (const auto& channel : gltfAnim.channels)
        {
            if (!channel.nodeIndex.has_value())
                continue;
            int targetNode = static_cast<int>(channel.nodeIndex.value());

            const auto& sampler = gltfAnim.samplers[channel.samplerIndex];

            // Issue #41: honour the sampler's interpolation mode (it used to
            // be ignored and every track sampled as linear).
            InterpolationMode interpolation = InterpolationMode::Linear;
            if (sampler.interpolation == fastgltf::AnimationInterpolation::Step)
                interpolation = InterpolationMode::Step;
            else if (sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline)
                interpolation = InterpolationMode::CubicSpline;

            // Extract times (one per key, also for CUBICSPLINE).
            std::vector<float> times;
            auto& timeAccessor = asset.accessors[sampler.inputAccessor];
            fastgltf::iterateAccessor<float>(asset, timeAccessor,
                                             [&](float t)
                                             {
                                                 times.push_back(t);
                                                 clip.Duration = std::max(clip.Duration, t);
                                             });
            if (times.empty())
                continue;

            auto& valueAccessor = asset.accessors[sampler.outputAccessor];

            if (channel.path == fastgltf::AnimationPath::Translation)
            {
                VectorTrack* track = FindOrAddTrack(clip.PositionTracks, targetNode);
                track->Interpolation = interpolation;
                AppendTrackKeys<glm::vec3, fastgltf::math::fvec3>(
                    asset, valueAccessor, times, interpolation, track->Keyframes, track->InTangents,
                    track->OutTangents,
                    [](const fastgltf::math::fvec3& v) { return glm::vec3(v.x(), v.y(), v.z()); });
            }
            else if (channel.path == fastgltf::AnimationPath::Rotation)
            {
                QuaternionTrack* track = FindOrAddTrack(clip.RotationTracks, targetNode);
                track->Interpolation = interpolation;
                AppendTrackKeys<glm::quat, fastgltf::math::fvec4>(
                    asset, valueAccessor, times, interpolation, track->Keyframes, track->InTangents,
                    track->OutTangents,
                    [](const fastgltf::math::fvec4& v)
                    {
                        // glTF rotation is x,y,z,w. GLM quat constructor takes w,x,y,z.
                        return glm::normalize(glm::quat(v.w(), v.x(), v.y(), v.z()));
                    });
            }
            else if (channel.path == fastgltf::AnimationPath::Scale)
            {
                VectorTrack* track = FindOrAddTrack(clip.ScaleTracks, targetNode);
                track->Interpolation = interpolation;
                AppendTrackKeys<glm::vec3, fastgltf::math::fvec3>(
                    asset, valueAccessor, times, interpolation, track->Keyframes, track->InTangents,
                    track->OutTangents,
                    [](const fastgltf::math::fvec3& v) { return glm::vec3(v.x(), v.y(), v.z()); });
            }
        }

        m_Animations.push_back(clip);
    }
}

} // namespace UHE::RD3d
