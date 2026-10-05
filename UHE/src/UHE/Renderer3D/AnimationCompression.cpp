#include "AnimationCompression.h"
#include "UHE/Jobsystem/Jobsystem.h"
#include <cmath>
#include <functional>
#include <type_traits>
#include <vector>

namespace UHE::RD3d {

namespace
{

float LinearBlend(float a, float b, float u) { return a + (b - a) * u; }
glm::vec3 LinearBlend(const glm::vec3& a, const glm::vec3& b, float u) { return glm::mix(a, b, u); }
glm::quat LinearBlend(const glm::quat& a, const glm::quat& b, float u) { return glm::slerp(a, b, u); }

// Angle between two rotations, in radians.
float TrackError(const glm::quat& a, const glm::quat& b)
{
    float d = glm::clamp(std::abs(glm::dot(a, b)), 0.0f, 1.0f);
    return 2.0f * std::acos(d);
}

float TrackError(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b); }

// Value the track takes at 'time' when jumping straight from key a to key b:
// linear for Linear tracks, one long Hermite segment for CubicSpline tracks
// (using the surviving keys' tangents).
template <typename T>
T SampleBetween(const Keyframe<T>& a, const Keyframe<T>& b, const T& aOutTangent, const T& bInTangent, bool spline,
                float time)
{
    float span = b.Time - a.Time;
    float u = span > 0.0f ? glm::clamp((time - a.Time) / span, 0.0f, 1.0f) : 0.0f;
    if (!spline)
        return LinearBlend(a.Value, b.Value, u);

    float t = u * span; // local segment parameter
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    float h11 = t3 - t2;
    T value = h00 * a.Value + h10 * span * aOutTangent + h01 * b.Value + h11 * span * bInTangent;
    if constexpr (std::is_same_v<T, glm::quat>)
        return glm::normalize(value);
    else
        return value;
}

// Greedy linear key reduction: walks the track once, keeping a key when the
// straight blend between the last kept key and the next original key would
// deviate from the original value by more than 'tolerance'. The max deviation
// of the reduced track from the original is bounded by 'tolerance' at the
// original sample times. Returns the indices to keep (always includes the
// first and last key).
template <typename T>
std::vector<size_t> ComputeKeepMask(const std::vector<Keyframe<T>>& keys, const std::vector<T>& inTangents,
                                    const std::vector<T>& outTangents, bool spline, float tolerance)
{
    std::vector<size_t> keep;
    if (keys.empty())
        return keep;
    if (keys.size() <= 2)
    {
        keep.resize(keys.size());
        for (size_t i = 0; i < keys.size(); ++i)
            keep[i] = i;
        return keep;
    }

    const T emptyTangent{}; // zero vec / identity quat fallback for missing tangents
    auto tangentAt = [&](const std::vector<T>& tangents, size_t i) -> const T&
    {
        return i < tangents.size() ? tangents[i] : emptyTangent;
    };

    keep.push_back(0);
    size_t lastKept = 0;
    size_t i = 1;
    while (i + 1 < keys.size())
    {
        const Keyframe<T>& a = keys[lastKept];
        const Keyframe<T>& b = keys[i + 1];
        T blended =
            SampleBetween(a, b, tangentAt(outTangents, lastKept), tangentAt(inTangents, i + 1), spline, keys[i].Time);
        if (TrackError(keys[i].Value, blended) <= tolerance)
        {
            ++i; // drop key i
        }
        else
        {
            keep.push_back(i);
            lastKept = i;
            ++i;
        }
    }
    keep.push_back(keys.size() - 1);
    return keep;
}

template <typename T>
void ApplyMask(std::vector<Keyframe<T>>& keys, std::vector<T>& inTangents, std::vector<T>& outTangents,
               const std::vector<size_t>& keep)
{
    auto filter = [&](auto& values)
    {
        if (values.empty())
            return;
        std::decay_t<decltype(values)> out;
        out.reserve(keep.size());
        for (size_t index : keep)
            out.push_back(values[index]);
        values = std::move(out);
    };
    filter(keys);
    filter(inTangents);
    filter(outTangents);
}

// Reduces one track (any of the three path types). 'reduce' is false for STEP
// tracks, whose step semantics would change if interior keys were dropped.
template <typename T>
CompressionStats ProcessTrack(std::vector<Keyframe<T>>& keys, std::vector<T>& inTangents,
                              std::vector<T>& outTangents, float tolerance, bool reduce, bool apply)
{
    CompressionStats stats;
    stats.KeysBefore = keys.size();
    stats.TracksProcessed = 1;

    const bool spline = !inTangents.empty() && inTangents.size() == keys.size() &&
                        outTangents.size() == keys.size();
    const size_t keyBytes = sizeof(Keyframe<T>);
    const size_t tangentBytes = sizeof(T);
    stats.BytesBefore = keys.size() * keyBytes + (spline ? 2 * keys.size() * tangentBytes : 0);
    stats.KeysAfter = keys.size();
    stats.BytesAfter = stats.BytesBefore;

    if (reduce && keys.size() > 2)
    {
        std::vector<size_t> keep = ComputeKeepMask(keys, inTangents, outTangents, spline, tolerance);
        stats.KeysAfter = keep.size();
        stats.BytesAfter = keep.size() * keyBytes + (spline ? 2 * keep.size() * tangentBytes : 0);
        if (apply && keep.size() < keys.size())
            ApplyMask(keys, inTangents, outTangents, keep);
    }

    return stats;
}

CompressionStats MergeStats(const std::vector<CompressionStats>& parts)
{
    CompressionStats total;
    for (const auto& part : parts)
    {
        total.KeysBefore += part.KeysBefore;
        total.KeysAfter += part.KeysAfter;
        total.TracksProcessed += part.TracksProcessed;
        total.BytesBefore += part.BytesBefore;
        total.BytesAfter += part.BytesAfter;
    }
    return total;
}

CompressionStats RunReduction(AnimationClip& clip, const CompressSettings& settings,
                              Jobsystem::UheJobsystem* jobs, bool apply)
{
    std::vector<std::function<CompressionStats()>> work;

    for (auto& track : clip.PositionTracks)
    {
        bool reduce = track.Interpolation != InterpolationMode::Step;
        work.push_back([&track, &settings, reduce, apply] {
            return ProcessTrack<glm::vec3>(track.Keyframes, track.InTangents, track.OutTangents,
                                           settings.PositionTolerance, reduce, apply);
        });
    }
    for (auto& track : clip.RotationTracks)
    {
        bool reduce = track.Interpolation != InterpolationMode::Step;
        work.push_back([&track, &settings, reduce, apply] {
            return ProcessTrack<glm::quat>(track.Keyframes, track.InTangents, track.OutTangents,
                                           settings.RotationTolerance, reduce, apply);
        });
    }
    for (auto& track : clip.ScaleTracks)
    {
        bool reduce = track.Interpolation != InterpolationMode::Step;
        work.push_back([&track, &settings, reduce, apply] {
            return ProcessTrack<glm::vec3>(track.Keyframes, track.InTangents, track.OutTangents,
                                           settings.ScaleTolerance, reduce, apply);
        });
    }

    std::vector<CompressionStats> results(work.size());
    auto run = [&](u32 i)
    {
        results[i] = work[i]();
    };

    if (jobs && !work.empty())
        jobs->ParallelFor(static_cast<u32>(work.size()), run);
    else
        for (u32 i = 0; i < work.size(); ++i)
            run(i);

    return MergeStats(results);
}

} // namespace

CompressionStats AnimationCompressor::CompressClip(AnimationClip& clip, const CompressSettings& settings,
                                                   Jobsystem::UheJobsystem* jobs)
{
    return RunReduction(clip, settings, jobs, /*apply=*/true);
}

CompressionStats AnimationCompressor::MeasureClip(const AnimationClip& clip, const CompressSettings& settings)
{
    AnimationClip copy = clip;
    return RunReduction(copy, settings, /*jobs=*/nullptr, /*apply=*/false);
}

} // namespace UHE::RD3d
