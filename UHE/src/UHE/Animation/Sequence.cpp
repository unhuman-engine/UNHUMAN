#include "UHE/Animation/Sequence.h"
#include <algorithm>
#include <cmath>

namespace UHE
{
namespace
{
constexpr f32 kSameKeyTime = 0.0005f;

/// Catmull-Rom between p1 and p2 at u in [0, 1], tangents from the neighbours.
/// Uneven key spacing scales each tangent by its segment, so a key close to
/// its neighbour does not overshoot.
glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, f32 t0, f32 t1,
                     f32 t2, f32 t3, f32 u)
{
    const f32 span = std::max(t2 - t1, 1e-6f);
    const glm::vec3 m1 = (p2 - p0) / std::max(t2 - t0, 1e-6f) * span;
    const glm::vec3 m2 = (p3 - p1) / std::max(t3 - t1, 1e-6f) * span;
    const f32 u2 = u * u;
    const f32 u3 = u2 * u;
    return (2.0f * u3 - 3.0f * u2 + 1.0f) * p1 + (u3 - 2.0f * u2 + u) * m1 + (-2.0f * u3 + 3.0f * u2) * p2 +
           (u3 - u2) * m2;
}

void CollectEvents(const SequenceTrack& track, f32 from, f32 to, std::vector<std::pair<f32, std::string>>& out)
{
    for (const SequenceKey& key : track.Keys)
    {
        if (key.Time > from && key.Time <= to && !key.Event.empty())
            out.emplace_back(key.Time, key.Event);
    }
}
} // namespace

glm::vec3 Sequence::Sample(const SequenceTrack& track, f32 time, const glm::vec3& fallback)
{
    const std::vector<SequenceKey>& keys = track.Keys;
    if (keys.empty())
        return fallback;
    if (time <= keys.front().Time)
        return keys.front().Value;
    if (time >= keys.back().Time)
        return keys.back().Value;

    // The segment [i, i + 1] holding time.
    const auto next =
        std::upper_bound(keys.begin(), keys.end(), time, [](f32 t, const SequenceKey& key) { return t < key.Time; });
    const std::size_t i = static_cast<std::size_t>(next - keys.begin()) - 1;
    const SequenceKey& a = keys[i];
    const SequenceKey& b = keys[i + 1];
    const f32 span = b.Time - a.Time;
    const f32 u = span > 1e-6f ? (time - a.Time) / span : 1.0f;
    switch (a.Interp)
    {
        case SequenceInterp::Step:
            return a.Value;
        case SequenceInterp::Linear:
            return glm::mix(a.Value, b.Value, u);
        case SequenceInterp::Smooth:
        {
            // At the ends the missing neighbour mirrors the key: zero slope is
            // not forced, but the curve does not run past the end key either.
            const SequenceKey& before = i > 0 ? keys[i - 1] : a;
            const SequenceKey& after = i + 2 < keys.size() ? keys[i + 2] : b;
            return CatmullRom(before.Value, a.Value, b.Value, after.Value, i > 0 ? before.Time : a.Time - span, a.Time,
                              b.Time, i + 2 < keys.size() ? after.Time : b.Time + span, u);
        }
    }
    return a.Value;
}

void Sequence::SortKeys()
{
    for (SequenceTrack& track : Tracks)
    {
        std::stable_sort(track.Keys.begin(), track.Keys.end(),
                         [](const SequenceKey& lhs, const SequenceKey& rhs) { return lhs.Time < rhs.Time; });
    }
}

std::size_t Sequence::SetKey(SequenceTrack& track, f32 time, const glm::vec3& value, SequenceInterp interp)
{
    for (std::size_t i = 0; i < track.Keys.size(); ++i)
    {
        if (std::fabs(track.Keys[i].Time - time) <= kSameKeyTime)
        {
            track.Keys[i].Value = value;
            return i;
        }
    }
    const auto at = std::upper_bound(track.Keys.begin(), track.Keys.end(), time,
                                     [](f32 t, const SequenceKey& key) { return t < key.Time; });
    SequenceKey key;
    key.Time = time;
    key.Value = value;
    key.Interp = interp;
    return static_cast<std::size_t>(track.Keys.insert(at, key) - track.Keys.begin());
}

void Sequence::Advance(f32& time, bool& playing, f32 delta, std::vector<std::string>* events) const
{
    const f32 duration = std::max(Duration, 1e-3f);
    if (!playing)
        return;
    if (delta < 0.0f)
    {
        time = Loop ? std::fmod(std::fmod(time + delta, duration) + duration, duration) : std::max(0.0f, time + delta);
        return;
    }

    std::vector<std::pair<f32, std::string>> fired;
    const auto collect = [&](f32 from, f32 to, f32 offset)
    {
        if (events == nullptr)
            return;
        std::vector<std::pair<f32, std::string>> part;
        for (const SequenceTrack& track : Tracks)
        {
            if (track.Property == SequenceProperty::Event && !track.Muted)
                CollectEvents(track, from, to, part);
        }
        for (auto& [t, name] : part)
            fired.emplace_back(t + offset, std::move(name));
    };

    // Events fire for keys in (from, to]; a playhead at 0 also fires a key at
    // exactly 0, so a sequence's first event is not skipped when it starts.
    f32 start = time;
    f32 remaining = delta;
    f32 offset = 0.0f;
    bool fromStart = start <= 0.0f;
    while (true)
    {
        const f32 to = start + remaining;
        const f32 lower = fromStart ? -1.0f : start;
        if (to < duration)
        {
            collect(lower, to, offset);
            time = to;
            break;
        }
        collect(lower, duration, offset);
        if (!Loop)
        {
            time = duration;
            playing = false;
            break;
        }
        // Wrap; a huge delta (a hitch) passes the whole timeline at most once more.
        remaining = std::min(to - duration, duration);
        offset += duration; // events after the wrap sort after those before it
        start = 0.0f;
        fromStart = true;
        if (remaining <= 0.0f)
        {
            time = 0.0f;
            break;
        }
    }
    if (events != nullptr)
    {
        std::stable_sort(fired.begin(), fired.end(),
                         [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        for (auto& [t, name] : fired)
            events->push_back(std::move(name));
    }
}

namespace
{
constexpr const char* kPropertyNames[] = {"Position", "Rotation", "Scale", "Event"};
constexpr const char* kInterpNames[] = {"Step", "Linear", "Smooth"};
} // namespace

const char* SequencePropertyName(SequenceProperty property)
{
    const int index = static_cast<int>(property);
    return index >= 0 && index < 4 ? kPropertyNames[index] : "Position";
}

bool SequencePropertyFromName(const std::string& name, SequenceProperty& out)
{
    for (int i = 0; i < 4; ++i)
    {
        if (name == kPropertyNames[i])
        {
            out = static_cast<SequenceProperty>(i);
            return true;
        }
    }
    return false;
}

const char* SequenceInterpName(SequenceInterp interp)
{
    const int index = static_cast<int>(interp);
    return index >= 0 && index < 3 ? kInterpNames[index] : "Smooth";
}

bool SequenceInterpFromName(const std::string& name, SequenceInterp& out)
{
    for (int i = 0; i < 3; ++i)
    {
        if (name == kInterpNames[i])
        {
            out = static_cast<SequenceInterp>(i);
            return true;
        }
    }
    return false;
}

} // namespace UHE
