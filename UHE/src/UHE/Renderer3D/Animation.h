#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE::RD3d
{

// Issue #41: how the runtime steps between keyframes. Mirrors the glTF
// sampler modes; the importer records the source sampler's mode per track.
enum class UHE_API InterpolationMode : unsigned char
{
    Step,
    Linear,
    CubicSpline
};

// Issue #41: playback looping policy. Loop is the default so clips authored
// before this system keep playing the way they always have.
enum class UHE_API LoopMode : unsigned char
{
    None,     // stop at the end
    Loop,     // wrap around
    PingPong  // reverse direction at each end
};

struct UHE_API Bone
{
    std::string Name;
    int ID = -1;
    int ParentID = -1;
    glm::mat4 InverseBindMatrix{1.0f};
    glm::mat4 LocalTransform{1.0f};
};

struct UHE_API Skeleton
{
    std::vector<Bone> Bones;
    std::vector<int> JointNodes;
    int RootBoneID = -1;
};

// Issue #41: one skinned joint set. A model may carry several skins; each
// references joints by glTF node index and owns its inverse bind matrices.
struct UHE_API Skin
{
    std::string Name;
    std::vector<int> JointNodes;
    std::vector<glm::mat4> InverseBindMatrices; // parallel to JointNodes
};

template <typename T> struct UHE_API Keyframe
{
    float Time;
    T Value;
};

struct UHE_API VectorTrack
{
    int TargetBoneID = -1;
    InterpolationMode Interpolation = InterpolationMode::Linear;
    std::vector<Keyframe<glm::vec3>> Keyframes;
    // CubicSpline only, parallel to Keyframes: Hermite tangents from the
    // glTF [outTangent_i, value_i, inTangent_{i+1}] output layout. Empty for
    // Step/Linear tracks so those stay compact.
    std::vector<glm::vec3> InTangents;
    std::vector<glm::vec3> OutTangents;
};

struct UHE_API QuaternionTrack
{
    int TargetBoneID = -1;
    InterpolationMode Interpolation = InterpolationMode::Linear;
    std::vector<Keyframe<glm::quat>> Keyframes;
    std::vector<glm::quat> InTangents;
    std::vector<glm::quat> OutTangents;
};

// Issue #41: notifies fired by the Animator when the sample time crosses
// Time while playing. Payload is free-form for gameplay code.
struct UHE_API AnimationEvent
{
    float Time = 0.0f;
    std::string Name;
};

struct UHE_API AnimationClip
{
    std::string Name;
    float Duration = 0.0f;
    LoopMode Loop = LoopMode::Loop;

    std::vector<VectorTrack> PositionTracks;
    std::vector<QuaternionTrack> RotationTracks;
    std::vector<VectorTrack> ScaleTracks;
    std::vector<AnimationEvent> Events;
};

// Sampled local pose of one bone, consumed by the Animator when blending.
struct UHE_API BonePose
{
    glm::vec3 Translation{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};
    bool Animated = false; // true when at least one track targets this bone
};

// Per-bone pose table for one sampled moment of a clip, indexed by bone ID.
struct UHE_API AnimationPose
{
    std::vector<BonePose> Bones;

    void Resize(size_t boneCount)
    {
        Bones.resize(boneCount);
        for (auto& bone : Bones)
        {
            bone.Translation = glm::vec3(0.0f);
            bone.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            bone.Scale = glm::vec3(1.0f);
            bone.Animated = false;
        }
    }
};

} // namespace UHE::RD3d
