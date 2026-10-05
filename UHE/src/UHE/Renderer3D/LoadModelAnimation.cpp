// Skin and animation parsing.
//
// Separate from the scene walk because it answers a different question: the
// walk places geometry, this builds the deformation data that Animator applies
// on top. Split out so the geometry path recompiles independently of it.

#include "uhepch.h"
#include "LoadModel.h"
#include "MeshoptDecode.h"
#include "fastgltf/math.hpp"
#include "fastgltf/tools.hpp"
#include "fastgltf/types.hpp"

namespace UHE::RD3d
{

namespace
{

// Same rationale as LoadModelGeometry.cpp: meshopt-compressed buffer views
// need on-the-fly decoding, and animations are commonly compressed in meshopt
// exports because their keyframe streams dominate file size.
struct MeshoptBufferDataAdapter
{
    const fastgltf::Asset* asset = nullptr;
    std::shared_ptr<std::vector<std::byte>> scratch = std::make_shared<std::vector<std::byte>>();

    std::span<const std::byte> operator()(const fastgltf::Asset& a, std::size_t bufferViewIndex) const
    {
        return GetBufferViewBytes(a, bufferViewIndex, *scratch);
    }
};

} // namespace

void Model::ParseSkins(const fastgltf::Asset& asset)
{
    if (asset.skins.empty())
        return;

    // For now, only parse the first skin
    const auto& skin = asset.skins[0];

    m_Skeleton.Bones.resize(asset.nodes.size()); // Map glTF nodes to bones directly for simplicity
    m_Skeleton.RootBoneID = skin.skeleton.value_or(skin.joints.empty() ? -1 : static_cast<int>(skin.joints[0]));

    // 1. Build hierarchy mapping from asset.nodes
    for (size_t i = 0; i < asset.nodes.size(); ++i)
    {
        const auto& node = asset.nodes[i];
        m_Skeleton.Bones[i].ID = static_cast<int>(i);
        m_Skeleton.Bones[i].Name = node.name.empty() ? "Bone_" + std::to_string(i) : std::string(node.name);

        m_Skeleton.Bones[i].LocalTransform = NodeLocalTransform(node);

        // Set parent
        for (auto childIdx : node.children)
        {
            m_Skeleton.Bones[childIdx].ParentID = static_cast<int>(i);
        }
    }

    m_Skeleton.JointNodes.assign(skin.joints.begin(), skin.joints.end());

    // 2. Extract Inverse Bind Matrices
    if (skin.inverseBindMatrices.has_value())
    {
        auto& ibmAccessor = asset.accessors[skin.inverseBindMatrices.value()];
        size_t jointIdx = 0;
        fastgltf::iterateAccessor<fastgltf::math::fmat4x4>(asset, ibmAccessor, [&](const fastgltf::math::fmat4x4& matrix) {
            if (jointIdx < skin.joints.size()) {
                size_t nodeIdx = skin.joints[jointIdx];
                std::memcpy(&m_Skeleton.Bones[nodeIdx].InverseBindMatrix, matrix.data(), sizeof(glm::mat4));
            }
            jointIdx++;
        }, MeshoptBufferDataAdapter{&asset});
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
            if (!channel.nodeIndex.has_value()) continue;
            int targetNode = static_cast<int>(channel.nodeIndex.value());

            if (channel.samplerIndex >= gltfAnim.samplers.size())
                continue;

            const auto& sampler = gltfAnim.samplers[channel.samplerIndex];

            // Extract times
            std::vector<float> times;
            auto& timeAccessor = asset.accessors[sampler.inputAccessor];
            fastgltf::iterateAccessor<float>(asset, timeAccessor, [&](float t) {
                times.push_back(t);
                clip.Duration = std::max(clip.Duration, t);
            }, MeshoptBufferDataAdapter{&asset});

            // Extract values
            auto& valueAccessor = asset.accessors[sampler.outputAccessor];

            if (channel.path == fastgltf::AnimationPath::Translation)
            {
                VectorTrack track;
                track.TargetBoneID = targetNode;
                size_t idx = 0;
                fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, valueAccessor, [&](fastgltf::math::fvec3 v) {
                    // A channel whose value count exceeds its time count would read
                    // past `times`; clamp rather than overrun.
                    if (idx >= times.size()) return;
                    track.Keyframes.push_back({times[idx++], glm::vec3(v.x(), v.y(), v.z())});
                }, MeshoptBufferDataAdapter{&asset});
                clip.PositionTracks.push_back(track);
            }
            else if (channel.path == fastgltf::AnimationPath::Rotation)
            {
                QuaternionTrack track;
                track.TargetBoneID = targetNode;
                size_t idx = 0;
                fastgltf::iterateAccessor<fastgltf::math::fvec4>(asset, valueAccessor, [&](fastgltf::math::fvec4 v) {
                    if (idx >= times.size()) return;
                    // glTF rotation is x,y,z,w. GLM quat constructor takes w,x,y,z.
                    track.Keyframes.push_back({times[idx++], glm::normalize(glm::quat(v.w(), v.x(), v.y(), v.z()))});
                }, MeshoptBufferDataAdapter{&asset});
                clip.RotationTracks.push_back(track);
            }
            else if (channel.path == fastgltf::AnimationPath::Scale)
            {
                VectorTrack track;
                track.TargetBoneID = targetNode;
                size_t idx = 0;
                fastgltf::iterateAccessor<fastgltf::math::fvec3>(asset, valueAccessor, [&](fastgltf::math::fvec3 v) {
                    if (idx >= times.size()) return;
                    track.Keyframes.push_back({times[idx++], glm::vec3(v.x(), v.y(), v.z())});
                }, MeshoptBufferDataAdapter{&asset});
                clip.ScaleTracks.push_back(track);
            }
        }

        m_Animations.push_back(clip);
    }
}

} // namespace UHE::RD3d
