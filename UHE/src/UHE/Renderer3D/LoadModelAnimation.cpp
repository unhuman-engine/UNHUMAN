// Skin and animation parsing.
//
// Separate from the scene walk because it answers a different question: the
// walk places geometry, this builds the deformation data that Animator applies
// on top. Split out so the geometry path recompiles independently of it.

#include "uhepch.h"
#include <functional>
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

    // Extract unified bone hierarchy across all nodes; individual skins bind
    // specific joint subsets and inverse bind matrices.
    m_Skeleton.Bones.resize(asset.nodes.size());
    m_Skeleton.RootBoneID = asset.skins[0].skeleton.has_value()
                                ? static_cast<i32>(asset.skins[0].skeleton.value())
                                : (asset.skins[0].joints.empty() ? -1 : static_cast<i32>(asset.skins[0].joints[0]));

    // 1. Build hierarchy mapping from asset.nodes
    for (size_t i = 0; i < asset.nodes.size(); ++i)
    {
        const auto& node = asset.nodes[i];
        m_Skeleton.Bones[i].ID = static_cast<i32>(i);
        m_Skeleton.Bones[i].Name = node.name.empty() ? "Bone_" + std::to_string(i) : std::string(node.name);

        m_Skeleton.Bones[i].LocalTransform = NodeLocalTransform(node);

        // Set parent
        for (auto childIdx : node.children)
        {
            m_Skeleton.Bones[childIdx].ParentID = static_cast<i32>(i);
        }
    }

    // Rest pose of every node, from the scene root down: what the joints are
    // when no animation moves them. Memoized, because a parent can come after
    // its child in the node list.
    std::vector<glm::mat4> restWorld(m_Skeleton.Bones.size(), glm::mat4(1.0f));
    std::vector<char> restDone(m_Skeleton.Bones.size(), 0);
    const std::function<const glm::mat4&(i32)> worldOf = [&](i32 bone) -> const glm::mat4&
    {
        if (!restDone[bone])
        {
            const i32 parent = m_Skeleton.Bones[bone].ParentID;
            restWorld[bone] = parent >= 0 ? worldOf(parent) * m_Skeleton.Bones[bone].LocalTransform
                                          : m_Skeleton.Bones[bone].LocalTransform;
            restDone[bone] = 1;
        }
        return restWorld[bone];
    };

    // 2. Every skin: joint list + inverse bind matrices.
    for (size_t s = 0; s < asset.skins.size(); ++s)
    {
        const auto& gltfSkin = asset.skins[s];
        Skin skin;
        skin.Name = gltfSkin.name.empty() ? "Skin_" + std::to_string(s) : std::string(gltfSkin.name);
        skin.JointNodes.clear();
        skin.JointNodes.reserve(gltfSkin.joints.size());
        for (auto joint : gltfSkin.joints)
            skin.JointNodes.push_back(static_cast<i32>(joint));
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
                                                               }, MeshoptBufferDataAdapter{&asset});
        }

        skin.RestPalette.resize(skin.JointNodes.size(), glm::mat4(1.0f));
        for (size_t j = 0; j < skin.JointNodes.size(); ++j)
        {
            const i32 joint = skin.JointNodes[j];
            if (joint >= 0 && joint < static_cast<i32>(m_Skeleton.Bones.size()))
                skin.RestPalette[j] = worldOf(joint) * skin.InverseBindMatrices[j];
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
static TrackT* FindOrAddTrack(std::vector<TrackT>& tracks, i32 targetBone)
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
template <typename T, typename GltfT, typename ToGlm, typename ToTangent>
static void AppendTrackKeys(const fastgltf::Asset& asset, const fastgltf::Accessor& valueAccessor,
                            const std::vector<f32>& times, InterpolationMode interpolation,
                            std::vector<Keyframe<T>>& keys, std::vector<T>& inTangents,
                            std::vector<T>& outTangents, ToGlm toGlm, ToTangent toTangent)
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
                                                          inTangents[key] = toTangent(v);
                                                      else if (part == 1)
                                                          keys[key] = {times[key], toGlm(v)};
                                                      else
                                                          outTangents[key] = toTangent(v);
                                                  }, MeshoptBufferDataAdapter{&asset});
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
                                         }, MeshoptBufferDataAdapter{&asset});
    }
}

template <typename T, typename GltfT, typename ToGlm>
static void AppendTrackKeys(const fastgltf::Asset& asset, const fastgltf::Accessor& valueAccessor,
                            const std::vector<f32>& times, InterpolationMode interpolation,
                            std::vector<Keyframe<T>>& keys, std::vector<T>& inTangents,
                            std::vector<T>& outTangents, ToGlm toGlm)
{
    AppendTrackKeys<T, GltfT>(asset, valueAccessor, times, interpolation, keys, inTangents, outTangents, toGlm, toGlm);
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
            i32 targetNode = static_cast<i32>(channel.nodeIndex.value());

            if (channel.samplerIndex >= gltfAnim.samplers.size())
                continue;

            const auto& sampler = gltfAnim.samplers[channel.samplerIndex];

            // NOTE: Map sampler interpolation explicitly per track (Step, Linear, or CubicSpline).
            InterpolationMode interpolation = InterpolationMode::Linear;
            if (sampler.interpolation == fastgltf::AnimationInterpolation::Step)
                interpolation = InterpolationMode::Step;
            else if (sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline)
                interpolation = InterpolationMode::CubicSpline;

            // Extract times (one per key, also for CUBICSPLINE).
            std::vector<f32> times;
            auto& timeAccessor = asset.accessors[sampler.inputAccessor];
            fastgltf::iterateAccessor<f32>(asset, timeAccessor,
                                             [&](f32 t)
                                             {
                                                 times.push_back(t);
                                                 clip.Duration = std::max(clip.Duration, t);
                                             }, MeshoptBufferDataAdapter{&asset});
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
                        // Orientation keyframes must be normalized unit quaternions.
                        return glm::normalize(glm::quat(v.w(), v.x(), v.y(), v.z()));
                    },
                    [](const fastgltf::math::fvec4& v)
                    {
                        // glTF 2.0: CubicSpline rotation tangents represent derivatives
                        // and must NOT be normalized to unit length.
                        return glm::quat(v.w(), v.x(), v.y(), v.z());
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
