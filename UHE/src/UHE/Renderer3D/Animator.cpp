#include "Animator.h"
#include <algorithm>
#include <cfloat>
#include <glm/gtx/matrix_decompose.hpp>

namespace UHE::RD3d {

Animator::Animator(Ref<Model> model)
    : m_Model(model)
{
    if (m_Model && !m_Model->GetSkeleton().Bones.empty())
    {
        m_GlobalTransforms.resize(m_Model->GetSkeleton().Bones.size(), glm::mat4(1.0f));
        m_FinalBoneMatrices.resize(m_Model->GetSkeleton().JointNodes.size(), glm::mat4(1.0f));
    }
    BuildChildLists();
}

float Animator::GetDuration() const
{
    if (!m_Model || m_CurrentAnimationIndex < 0 ||
        m_CurrentAnimationIndex >= static_cast<int>(m_Model->GetAnimations().size()))
        return 0.0f;
    return m_Model->GetAnimations()[m_CurrentAnimationIndex].Duration;
}

float Animator::GetNormalizedTime() const
{
    float duration = GetDuration();
    if (duration <= 0.0f)
        return 0.0f;
    return m_CurrentTime / duration;
}

void Animator::Play()
{
    if (m_CurrentAnimationIndex == -1)
        return;
    // Restart clips that already ran to the end under LoopMode::None.
    float duration = GetDuration();
    if (!m_IsPlaying && duration > 0.0f && m_CurrentTime >= duration - FLT_EPSILON && !m_Reversed)
        m_CurrentTime = 0.0f;
    m_IsPlaying = true;
}

void Animator::Pause()
{
    m_IsPlaying = false;
}

void Animator::Stop()
{
    m_IsPlaying = false;
    m_CurrentTime = 0.0f;
    m_PrevAnimationIndex = -1;
    m_BlendWeight = 1.0f;
    m_BlendDuration = 0.0f;
    m_BlendTime = 0.0f;
    m_RootMotionDelta = glm::vec3(0.0f);
}

void Animator::PlayAnimation(const std::string& name)
{
    if (!m_Model) return;
    const auto& animations = m_Model->GetAnimations();
    for (int i = 0; i < static_cast<int>(animations.size()); ++i)
    {
        if (animations[i].Name == name)
        {
            PlayAnimation(i);
            return;
        }
    }
}

void Animator::PlayAnimation(int index)
{
    if (!m_Model) return;
    const auto& animations = m_Model->GetAnimations();
    if (index >= 0 && index < static_cast<int>(animations.size()))
    {
        m_CurrentAnimationIndex = index;
        m_CurrentTime = 0.0f;
        m_PrevAnimationIndex = -1; // a hard cut cancels any running fade
        m_BlendWeight = 1.0f;
        m_IsPlaying = true;
    }
}

void Animator::CrossFade(const std::string& name, float duration)
{
    if (!m_Model) return;
    const auto& animations = m_Model->GetAnimations();
    for (int i = 0; i < static_cast<int>(animations.size()); ++i)
    {
        if (animations[i].Name == name)
        {
            CrossFade(i, duration);
            return;
        }
    }
}

void Animator::CrossFade(int index, float duration)
{
    if (!m_Model) return;
    if (index < 0 || index >= static_cast<int>(m_Model->GetAnimations().size()))
        return;

    if (m_CurrentAnimationIndex == -1)
    {
        PlayAnimation(index); // nothing to fade from
        return;
    }

    m_PrevAnimationIndex = m_CurrentAnimationIndex;
    m_PrevTime = m_CurrentTime;
    m_CurrentAnimationIndex = index;
    m_CurrentTime = 0.0f;
    m_BlendDuration = std::max(duration, FLT_EPSILON);
    m_BlendTime = 0.0f;
    m_BlendWeight = 0.0f;
    m_IsPlaying = true;
}

void Animator::SetSkin(int index)
{
    if (!m_Model)
        return;
    if (index < 0 || index >= static_cast<int>(m_Model->GetSkinCount()))
        return;
    m_SkinIndex = index;
    m_FinalBoneMatrices.assign(m_Model->GetSkin(m_SkinIndex)->JointNodes.size(), glm::mat4(1.0f));
}

void Animator::UpdateAnimation(float dt)
{
    m_RootMotionDelta = glm::vec3(0.0f);

    if (!m_Model || m_CurrentAnimationIndex == -1 || m_CurrentAnimationIndex >= static_cast<int>(m_Model->GetAnimations().size()) || m_Model->GetSkeleton().RootBoneID == -1)
        return;

    const auto& skeleton = m_Model->GetSkeleton();
    if (m_GlobalTransforms.size() != skeleton.Bones.size())
        m_GlobalTransforms.resize(skeleton.Bones.size(), glm::mat4(1.0f));

    if (m_FinalBoneMatrices.size() != skeleton.JointNodes.size())
        m_FinalBoneMatrices.resize(skeleton.JointNodes.size(), glm::mat4(1.0f));

    if (m_ChildBones.size() != skeleton.Bones.size())
        BuildChildLists();

    const AnimationClip& clip = m_Model->GetAnimations()[m_CurrentAnimationIndex];
    const float duration = clip.Duration;

    if (!m_IsPlaying || duration <= 0.0f)
        return;

    // --- advance time, respecting speed, direction and loop mode ---
    float effDt = dt * m_TimeScale;
    float direction = m_Reversed ? -1.0f : 1.0f;
    if (m_LoopMode == LoopMode::PingPong && !m_PingPongForward)
        direction = -direction;

    float prevTime = m_CurrentTime;
    float newTime = prevTime + effDt * direction;
    bool movedForward = direction > 0.0f;
    bool wrapped = false;

    if (m_LoopMode == LoopMode::None)
    {
        if (newTime >= duration) { newTime = duration; m_IsPlaying = false; }
        else if (newTime <= 0.0f) { newTime = 0.0f; m_IsPlaying = false; }
    }
    else if (m_LoopMode == LoopMode::Loop)
    {
        while (newTime >= duration) { newTime -= duration; wrapped = true; }
        while (newTime < 0.0f) { newTime += duration; wrapped = true; }
    }
    else // PingPong: reflect at the ends and flip direction
    {
        while (newTime >= duration)
        {
            newTime = 2.0f * duration - newTime;
            m_PingPongForward = !m_PingPongForward;
            wrapped = true;
        }
        while (newTime < 0.0f)
        {
            newTime = -newTime;
            m_PingPongForward = !m_PingPongForward;
            wrapped = true;
        }
    }
    m_CurrentTime = newTime;

    FireEvents(clip, prevTime, newTime, movedForward);

    if (m_RootMotionEnabled)
        m_RootMotionDelta = ExtractRootMotion(clip, prevTime, newTime, wrapped);

    // --- sample poses (current clip, and the fading-out clip if any) ---
    m_CurrentPose.Resize(skeleton.Bones.size());
    SampleClip(clip, m_CurrentTime, m_CurrentPose);

    if (m_PrevAnimationIndex != -1 && m_BlendDuration > 0.0f)
    {
        const AnimationClip& prevClip = m_Model->GetAnimations()[m_PrevAnimationIndex];
        // The fading-out clip keeps advancing; Loop semantics so it never stalls.
        m_PrevTime += effDt * direction;
        if (prevClip.Duration > 0.0f)
        {
            while (m_PrevTime >= prevClip.Duration) m_PrevTime -= prevClip.Duration;
            while (m_PrevTime < 0.0f) m_PrevTime += prevClip.Duration;
        }

        m_PrevPose.Resize(skeleton.Bones.size());
        SampleClip(prevClip, m_PrevTime, m_PrevPose);

        m_BlendTime += std::abs(effDt);
        float w = std::min(m_BlendTime / m_BlendDuration, 1.0f);
        m_BlendWeight = w * w * (3.0f - 2.0f * w); // smoothstep for a softer fade
        BlendPoses(m_PrevPose, m_CurrentPose, m_BlendWeight, m_BlendedPose);

        if (m_BlendWeight >= 1.0f)
        {
            m_PrevAnimationIndex = -1;
            m_BlendDuration = 0.0f;
        }
    }
    else
    {
        m_BlendedPose = m_CurrentPose;
    }

    CalculateBoneTransform(m_Model->GetSkeleton(), m_Model->GetSkeleton().RootBoneID, glm::mat4(1.0f));
    ComputeFinalMatrices();
}

void Animator::Seek(float time)
{
    if (!m_Model || m_CurrentAnimationIndex == -1 || m_Model->GetSkeleton().RootBoneID == -1)
        return;

    const auto& skeleton = m_Model->GetSkeleton();
    if (m_GlobalTransforms.size() != skeleton.Bones.size())
        m_GlobalTransforms.resize(skeleton.Bones.size(), glm::mat4(1.0f));
    if (m_FinalBoneMatrices.size() != skeleton.JointNodes.size())
        m_FinalBoneMatrices.resize(skeleton.JointNodes.size(), glm::mat4(1.0f));
    if (m_ChildBones.size() != skeleton.Bones.size())
        BuildChildLists();

    const AnimationClip& clip = m_Model->GetAnimations()[m_CurrentAnimationIndex];
    m_CurrentTime = clip.Duration > 0.0f ? std::clamp(time, 0.0f, clip.Duration) : 0.0f;

    m_CurrentPose.Resize(skeleton.Bones.size());
    SampleClip(clip, m_CurrentTime, m_CurrentPose);
    m_BlendedPose = m_CurrentPose; // scrubbing shows the target clip only

    CalculateBoneTransform(m_Model->GetSkeleton(), m_Model->GetSkeleton().RootBoneID, glm::mat4(1.0f));
    ComputeFinalMatrices();
}

void Animator::ComputeFinalMatrices()
{
    const auto& skeleton = m_Model->GetSkeleton();

    // --- final skinning matrices from the selected skin ---
    const Skin* skin = m_Model->GetSkin(m_SkinIndex);
    if (skin && !skin->JointNodes.empty() && skin->InverseBindMatrices.size() == skin->JointNodes.size())
    {
        if (m_FinalBoneMatrices.size() != skin->JointNodes.size())
            m_FinalBoneMatrices.resize(skin->JointNodes.size(), glm::mat4(1.0f));
        for (size_t i = 0; i < skin->JointNodes.size(); ++i)
        {
            int boneID = skin->JointNodes[i];
            m_FinalBoneMatrices[i] = m_GlobalTransforms[boneID] * skin->InverseBindMatrices[i];
        }
    }
    else
    {
        for (size_t i = 0; i < skeleton.JointNodes.size(); ++i)
        {
            int boneID = skeleton.JointNodes[i];
            m_FinalBoneMatrices[i] = m_GlobalTransforms[boneID] * skeleton.Bones[boneID].InverseBindMatrix;
        }
    }
}

void Animator::SampleClip(const AnimationClip& clip, float time, AnimationPose& pose) const
{
    for (const auto& track : clip.PositionTracks)
    {
        if (track.TargetBoneID < 0 || track.TargetBoneID >= static_cast<int>(pose.Bones.size()))
            continue;
        auto& bone = pose.Bones[track.TargetBoneID];
        bone.Translation = SamplePosition(time, track);
        bone.Animated = true;
    }
    for (const auto& track : clip.RotationTracks)
    {
        if (track.TargetBoneID < 0 || track.TargetBoneID >= static_cast<int>(pose.Bones.size()))
            continue;
        auto& bone = pose.Bones[track.TargetBoneID];
        bone.Rotation = SampleRotation(time, track);
        bone.Animated = true;
    }
    for (const auto& track : clip.ScaleTracks)
    {
        if (track.TargetBoneID < 0 || track.TargetBoneID >= static_cast<int>(pose.Bones.size()))
            continue;
        auto& bone = pose.Bones[track.TargetBoneID];
        bone.Scale = SampleScale(time, track);
        bone.Animated = true;
    }
}

void Animator::BlendPoses(const AnimationPose& from, const AnimationPose& to, float weight, AnimationPose& out)
{
    size_t count = std::min(from.Bones.size(), to.Bones.size());
    out.Bones.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        const BonePose& a = from.Bones[i];
        const BonePose& b = to.Bones[i];
        BonePose& result = out.Bones[i];
        result.Translation = glm::mix(a.Translation, b.Translation, weight);
        result.Rotation = glm::slerp(a.Rotation, b.Rotation, weight);
        result.Scale = glm::mix(a.Scale, b.Scale, weight);
        result.Animated = a.Animated || b.Animated;
    }
}

void Animator::CalculateBoneTransform(const Skeleton& skeleton, int boneID, const glm::mat4& parentTransform)
{
    const Bone& bone = skeleton.Bones[boneID];
    glm::mat4 nodeTransform = bone.LocalTransform;

    if (boneID >= 0 && boneID < static_cast<int>(m_BlendedPose.Bones.size()) && m_BlendedPose.Bones[boneID].Animated)
    {
        const BonePose& pose = m_BlendedPose.Bones[boneID];
        nodeTransform = glm::translate(glm::mat4(1.0f), pose.Translation) * glm::mat4_cast(pose.Rotation) *
                        glm::scale(glm::mat4(1.0f), pose.Scale);
    }

    glm::mat4 globalTransform = parentTransform * nodeTransform;

    // Root motion: when enabled the root joint stays at the origin and the
    // extracted delta is applied to the entity by gameplay code instead.
    if (m_RootMotionEnabled && boneID == m_Model->GetSkeleton().RootBoneID)
        globalTransform[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

    m_GlobalTransforms[boneID] = globalTransform;

    for (int child : m_ChildBones[boneID])
        CalculateBoneTransform(skeleton, child, globalTransform);
}

void Animator::FireEvents(const AnimationClip& clip, float prevTime, float newTime, bool movedForward)
{
    if (!m_EventCallback || clip.Events.empty())
        return;

    auto fire = [this](const AnimationEvent& e)
    {
        if (m_EventCallback)
            m_EventCallback(e);
    };

    if (m_LoopMode == LoopMode::Loop && movedForward)
    {
        if (newTime < prevTime) // wrapped
        {
            for (const auto& e : clip.Events)
                if (e.Time > prevTime && e.Time <= clip.Duration)
                    fire(e);
            for (const auto& e : clip.Events)
                if (e.Time >= 0.0f && e.Time <= newTime)
                    fire(e);
            return;
        }
        for (const auto& e : clip.Events)
            if (e.Time > prevTime && e.Time <= newTime)
                fire(e);
    }
    else if (m_LoopMode == LoopMode::Loop && !movedForward)
    {
        if (newTime > prevTime) // wrapped backwards
        {
            for (const auto& e : clip.Events)
                if (e.Time < prevTime && e.Time >= 0.0f)
                    fire(e);
            for (const auto& e : clip.Events)
                if (e.Time <= clip.Duration && e.Time >= newTime)
                    fire(e);
            return;
        }
        for (const auto& e : clip.Events)
            if (e.Time <= prevTime && e.Time > newTime)
                fire(e);
    }
    else
    {
        // None / PingPong: fire across the traversed window regardless of
        // direction; reflections are approximated by the final segment.
        float a = std::min(prevTime, newTime);
        float b = std::max(prevTime, newTime);
        for (const auto& e : clip.Events)
            if (e.Time >= a && e.Time <= b)
                fire(e);
    }
}

glm::vec3 Animator::ExtractRootMotion(const AnimationClip& clip, float prevTime, float newTime, bool wrapped) const
{
    const VectorTrack* rootTrack = nullptr;
    int rootBoneID = m_Model->GetSkeleton().RootBoneID;
    for (const auto& track : clip.PositionTracks)
    {
        if (track.TargetBoneID == rootBoneID)
        {
            rootTrack = &track;
            break;
        }
    }
    if (!rootTrack)
        return glm::vec3(0.0f);

    glm::vec3 prevPos = SamplePosition(prevTime, *rootTrack);
    glm::vec3 newPos = SamplePosition(newTime, *rootTrack);

    if (!wrapped)
        return newPos - prevPos;

    // Wrapped: accumulate the motion through the ends instead of jumping.
    glm::vec3 endPos = SamplePosition(clip.Duration, *rootTrack);
    glm::vec3 startPos = SamplePosition(0.0f, *rootTrack);
    if (newTime >= prevTime) // wrapped forwards
        return (endPos - prevPos) + (newPos - startPos);
    return (startPos - prevPos) + (endPos - newPos);
}

glm::vec3 Animator::SamplePosition(float time, const VectorTrack& track) const
{
    const auto& keys = track.Keyframes;
    if (keys.empty()) return glm::vec3(0.0f);
    if (keys.size() == 1) return keys[0].Value;

    auto it = std::upper_bound(keys.begin(), keys.end(), time,
                               [](float t, const Keyframe<glm::vec3>& k) { return t < k.Time; });
    if (it == keys.begin()) return keys.front().Value;
    if (it == keys.end()) return keys.back().Value;
    size_t i = static_cast<size_t>(it - keys.begin());

    if (track.Interpolation == InterpolationMode::Step)
        return keys[i - 1].Value;

    if (track.Interpolation == InterpolationMode::CubicSpline &&
        track.OutTangents.size() == keys.size() && track.InTangents.size() == keys.size())
    {
        glm::vec3 v = Hermite(keys[i - 1], keys[i], track.InTangents[i], track.OutTangents[i - 1], time);
        return v;
    }

    float span = keys[i].Time - keys[i - 1].Time;
    float u = span > 0.0f ? (time - keys[i - 1].Time) / span : 0.0f;
    return glm::mix(keys[i - 1].Value, keys[i].Value, u);
}

glm::quat Animator::SampleRotation(float time, const QuaternionTrack& track) const
{
    const auto& keys = track.Keyframes;
    if (keys.empty()) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (keys.size() == 1) return keys[0].Value;

    auto it = std::upper_bound(keys.begin(), keys.end(), time,
                               [](float t, const Keyframe<glm::quat>& k) { return t < k.Time; });
    if (it == keys.begin()) return keys.front().Value;
    if (it == keys.end()) return keys.back().Value;
    size_t i = static_cast<size_t>(it - keys.begin());

    if (track.Interpolation == InterpolationMode::Step)
        return keys[i - 1].Value;

    if (track.Interpolation == InterpolationMode::CubicSpline &&
        track.OutTangents.size() == keys.size() && track.InTangents.size() == keys.size())
    {
        // Component-wise Hermite, renormalised to stay a unit rotation.
        glm::quat q = Hermite(keys[i - 1], keys[i], track.InTangents[i], track.OutTangents[i - 1], time);
        return glm::normalize(q);
    }

    float span = keys[i].Time - keys[i - 1].Time;
    float u = span > 0.0f ? (time - keys[i - 1].Time) / span : 0.0f;
    return glm::slerp(keys[i - 1].Value, keys[i].Value, u);
}

glm::vec3 Animator::SampleScale(float time, const VectorTrack& track) const
{
    const auto& keys = track.Keyframes;
    if (keys.empty()) return glm::vec3(1.0f);
    if (keys.size() == 1) return keys[0].Value;

    auto it = std::upper_bound(keys.begin(), keys.end(), time,
                               [](float t, const Keyframe<glm::vec3>& k) { return t < k.Time; });
    if (it == keys.begin()) return keys.front().Value;
    if (it == keys.end()) return keys.back().Value;
    size_t i = static_cast<size_t>(it - keys.begin());

    if (track.Interpolation == InterpolationMode::Step)
        return keys[i - 1].Value;

    if (track.Interpolation == InterpolationMode::CubicSpline &&
        track.OutTangents.size() == keys.size() && track.InTangents.size() == keys.size())
    {
        return Hermite(keys[i - 1], keys[i], track.InTangents[i], track.OutTangents[i - 1], time);
    }

    float span = keys[i].Time - keys[i - 1].Time;
    float u = span > 0.0f ? (time - keys[i - 1].Time) / span : 0.0f;
    return glm::mix(keys[i - 1].Value, keys[i].Value, u);
}

template <typename T>
T Animator::Hermite(const Keyframe<T>& a, const Keyframe<T>& b, const T& inTangent, const T& outTangent, float time)
{
    float span = b.Time - a.Time;
    float t = span > 0.0f ? (time - a.Time) / span : 0.0f;
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    float h11 = t3 - t2;
    return h00 * a.Value + h10 * span * outTangent + h01 * b.Value + h11 * span * inTangent;
}

void Animator::BuildChildLists()
{
    if (!m_Model)
        return;
    const auto& skeleton = m_Model->GetSkeleton();
    m_ChildBones.assign(skeleton.Bones.size(), {});
    for (size_t i = 0; i < skeleton.Bones.size(); ++i)
    {
        int parent = skeleton.Bones[i].ParentID;
        if (parent >= 0 && parent < static_cast<int>(m_ChildBones.size()))
            m_ChildBones[parent].push_back(static_cast<int>(i));
    }
}

} // namespace UHE::RD3d
