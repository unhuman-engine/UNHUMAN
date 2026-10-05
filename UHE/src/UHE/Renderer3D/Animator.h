#pragma once
#include <functional>
#include <vector>
#include "UHE/Core/Core.h"
#include "UHE/Renderer3D/Animation.h"
#include "UHE/Renderer3D/LoadModel.h"

namespace UHE::RD3d
{

// Issue #41: animation runtime. On top of the old single-clip player this
// adds playback controls (speed, loop modes, reverse, pause), STEP/LINEAR/
// CUBICSPLINE sampling, cross-fade blending, animation events, root motion
// extraction and multi-skin support.
class UHE_API Animator
{
public:
    Animator() = default;
    Animator(Ref<Model> model);

    // ---- playback ----
    void UpdateAnimation(float dt);
    void PlayAnimation(const std::string& name);
    void PlayAnimation(int index);
    void Play();
    void Pause();
    void Stop();
    bool IsPlaying() const { return m_IsPlaying; }

    // Scales animation time; 1 = authored speed.
    void SetTimeScale(float scale) { m_TimeScale = scale; }
    float GetTimeScale() const { return m_TimeScale; }
    void SetLoopMode(LoopMode mode) { m_LoopMode = mode; }
    LoopMode GetLoopMode() const { return m_LoopMode; }
    void SetReversed(bool reversed) { m_Reversed = reversed; }
    bool IsReversed() const { return m_Reversed; }

    float GetCurrentTime() const { return m_CurrentTime; }
    float GetDuration() const;
    float GetNormalizedTime() const; // [0, 1], 0 when no clip
    // Jumps to an absolute time and re-evaluates the pose immediately, so
    // scrubbing works even while paused (editor timeline).
    void Seek(float time);

    // ---- cross-fade blending ----
    // Blends from the clip playing now into 'name' over 'duration' seconds.
    // Both clips keep advancing during the fade.
    void CrossFade(const std::string& name, float duration);
    void CrossFade(int index, float duration);
    bool IsBlending() const { return m_PrevAnimationIndex != -1; }
    float GetBlendWeight() const { return m_BlendWeight; } // 0 = old clip, 1 = new clip

    // ---- events / notifies ----
    using EventCallback = std::function<void(const AnimationEvent&)>;
    void SetEventCallback(EventCallback callback) { m_EventCallback = std::move(callback); }

    // ---- root motion ----
    // When enabled, the root joint's animated translation is stripped from the
    // final pose and reported through GetRootMotionDelta() so gameplay can
    // move the entity instead.
    void SetRootMotionEnabled(bool enable) { m_RootMotionEnabled = enable; }
    bool IsRootMotionEnabled() const { return m_RootMotionEnabled; }
    // Translation the root joint moved during the last UpdateAnimation, in
    // skeleton space. Zero while disabled.
    const glm::vec3& GetRootMotionDelta() const { return m_RootMotionDelta; }

    // ---- skins ----
    // Selects which of the model's skins drives the final bone matrices.
    void SetSkin(int index);
    int GetSkinIndex() const { return m_SkinIndex; }

    const std::vector<glm::mat4>& GetFinalBoneMatrices() const { return m_FinalBoneMatrices; }
    Ref<Model> GetModel() const { return m_Model; }
    bool HasAnimation() const { return m_CurrentAnimationIndex != -1; }
    int GetCurrentAnimationIndex() const { return m_CurrentAnimationIndex; }

private:
    void SampleClip(const AnimationClip& clip, float time, AnimationPose& pose) const;
    static void BlendPoses(const AnimationPose& from, const AnimationPose& to, float weight, AnimationPose& out);
    void CalculateBoneTransform(const Skeleton& skeleton, int boneID, const glm::mat4& parentTransform);
    // Fires events whose time was crossed between prevTime and newTime,
    // honouring play direction and loop wraps.
    void FireEvents(const AnimationClip& clip, float prevTime, float newTime, bool movedForward);
    // Extracts the root joint's translation change between two sample times.
    glm::vec3 ExtractRootMotion(const AnimationClip& clip, float prevTime, float newTime, bool wrapped) const;

    glm::vec3 SamplePosition(float time, const VectorTrack& track) const;
    glm::quat SampleRotation(float time, const QuaternionTrack& track) const;
    glm::vec3 SampleScale(float time, const VectorTrack& track) const;
    // Hermite evaluation for CubicSpline tracks.
    template <typename T>
    static T Hermite(const Keyframe<T>& a, const Keyframe<T>& b, const T& inTangent, const T& outTangent, float time);

    void BuildChildLists();
    // Multiplies the global transforms with the selected skin's inverse binds.
    void ComputeFinalMatrices();

private:
    Ref<Model> m_Model;
    int m_CurrentAnimationIndex = -1;
    float m_CurrentTime = 0.0f;
    bool m_IsPlaying = true;
    float m_TimeScale = 1.0f;
    LoopMode m_LoopMode = LoopMode::Loop;
    bool m_Reversed = false;
    bool m_PingPongForward = true; // bounce direction for LoopMode::PingPong

    int m_SkinIndex = 0;

    // Cross-fade state: m_PrevAnimationIndex >= 0 while a fade is running.
    int m_PrevAnimationIndex = -1;
    float m_PrevTime = 0.0f;
    float m_BlendDuration = 0.0f;
    float m_BlendTime = 0.0f;
    float m_BlendWeight = 1.0f;

    AnimationPose m_PrevPose;
    AnimationPose m_CurrentPose;
    AnimationPose m_BlendedPose;

    EventCallback m_EventCallback;

    bool m_RootMotionEnabled = false;
    glm::vec3 m_RootMotionDelta{0.0f};

    std::vector<std::vector<int>> m_ChildBones; // per bone: child bone IDs
    std::vector<glm::mat4> m_GlobalTransforms;
    std::vector<glm::mat4> m_FinalBoneMatrices;
};

} // namespace UHE::RD3d
