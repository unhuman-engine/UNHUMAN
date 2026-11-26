#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE::RD3d
{

/**
 * @brief Keyframe interpolation mode matching glTF 2.0 animation sampler specification.
 */
enum class UHE_API InterpolationMode : unsigned char
{
    Step,        ///< Instantaneous state change at keyframe time
    Linear,      ///< Linear / spherical linear interpolation between adjacent keys
    CubicSpline  ///< Cubic Hermite spline interpolation with in/out tangents
};

/**
 * @brief Playback wrap behavior when an animation reaches its duration boundary.
 */
enum class UHE_API LoopMode : unsigned char
{
    None,     ///< Clamp and stop at duration boundary
    Loop,     ///< Wrap to start and continue playing
    PingPong  ///< Invert playback direction upon hitting boundary
};

struct UHE_API Bone
{
    std::string Name;
    i32 ID = -1;
    i32 ParentID = -1;
    glm::mat4 InverseBindMatrix{1.0f};
    glm::mat4 LocalTransform{1.0f};
};

struct UHE_API Skeleton
{
    std::vector<Bone> Bones;
    std::vector<i32> JointNodes;
    i32 RootBoneID = -1;
};

/**
 * @brief Skinned mesh joint collection with inverse bind pose matrices.
 *
 * Models may define multiple skins; each skin references joint nodes
 * and provides parallel inverse bind matrices for GPU/CPU vertex skinning.
 */
struct UHE_API Skin
{
    std::string Name;
    std::vector<i32> JointNodes;
    std::vector<glm::mat4> InverseBindMatrices; ///< Parallel to JointNodes
    /// Joint world (rest pose) x inverse bind, parallel to JointNodes: the bone
    /// matrices when no clip plays, so a skinned model stands as its skeleton
    /// places it instead of falling back to its mesh node's transform.
    std::vector<glm::mat4> RestPalette;
};

template <typename T> struct UHE_API Keyframe
{
    f32 Time = 0.0f;
    T Value;
};

struct UHE_API VectorTrack
{
    i32 TargetBoneID = -1;
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
    i32 TargetBoneID = -1;
    InterpolationMode Interpolation = InterpolationMode::Linear;
    std::vector<Keyframe<glm::quat>> Keyframes;
    std::vector<glm::quat> InTangents;
    std::vector<glm::quat> OutTangents;
};

/**
 * @brief Timeline marker / notification fired when playback crosses its timestamp.
 */
struct UHE_API AnimationEvent
{
    f32 Time = 0.0f;
    std::string Name;
};

struct UHE_API AnimationClip
{
    std::string Name;
    f32 Duration = 0.0f;
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
