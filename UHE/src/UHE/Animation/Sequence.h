#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE
{
// ─── Sequencer (timeline) ───────────────────────────────────────────────
// A sequence is a fixed-length timeline of tracks. A track animates one
// property of one entity (named by UUID, so it survives a scene copy and a
// save) through keyframes, or carries named events that a script receives
// as `OnSequenceEvent(self, name)`. Cutscenes, doors, moving platforms,
// camera moves and scripted robot demos are sequences.

/// @brief What a track drives on its target entity.
enum class SequenceProperty : int
{
    Position = 0, ///< TransformComponent::Translation
    Rotation,     ///< TransformComponent::Rotation, Euler radians interpolated per axis
    Scale,        ///< TransformComponent::Scale
    Event,        ///< no value: each key fires its event name when the playhead passes it
};

/// @brief How the value moves from a key to the next one.
enum class SequenceInterp : int
{
    Step = 0, ///< holds the key's value until the next key
    Linear,   ///< straight line
    Smooth,   ///< Catmull-Rom through the neighbouring keys: no corner at a key
};

struct SequenceKey
{
    f32 Time = 0.0f;
    glm::vec3 Value{0.0f};
    SequenceInterp Interp = SequenceInterp::Smooth;
    std::string Event; ///< Event tracks only
};

struct SequenceTrack
{
    u64 Target = 0; ///< entity UUID; 0 or a missing entity makes the track do nothing
    SequenceProperty Property = SequenceProperty::Position;
    bool Muted = false;
    std::vector<SequenceKey> Keys; ///< sorted by time (SortKeys)
};

/// @brief A timeline of tracks; the logic only, no scene access (Scene::ApplySequence applies it).
struct UHE_API Sequence
{
    f32 Duration = 5.0f; ///< seconds; playback ends (or wraps) here
    bool Loop = false;
    std::vector<SequenceTrack> Tracks;

    /**
     * @brief The track's value at \p time.
     *
     * Before the first key it is the first key's value, after the last key
     * the last one's. A track without keys returns \p fallback.
     */
    [[nodiscard]] static glm::vec3 Sample(const SequenceTrack& track, f32 time, const glm::vec3& fallback);

    /// @brief Sorts every track's keys by time (stable, so equal times keep their order).
    void SortKeys();

    /**
     * @brief Sets a key at \p time: replaces a key within half a millisecond of it, or inserts one in order.
     * @return The key's index in the track.
     */
    static std::size_t SetKey(SequenceTrack& track, f32 time, const glm::vec3& value,
                              SequenceInterp interp = SequenceInterp::Smooth);

    /**
     * @brief Advances \p time by \p delta and reports what happened.
     *
     * Looping wraps the time into [0, Duration); otherwise it stops at
     * Duration and \p playing turns false. Events whose key lies in the
     * stretch of time passed, (from, to], are appended to \p events in order,
     * including those on both sides of a wrap. A negative delta (reverse
     * playback) moves back without firing events.
     */
    void Advance(f32& time, bool& playing, f32 delta, std::vector<std::string>* events) const;
};

/// @brief Display name of a property ("Position", ...).
[[nodiscard]] UHE_API const char* SequencePropertyName(SequenceProperty property);
/// @brief Property from its name; false when \p name names none.
UHE_API bool SequencePropertyFromName(const std::string& name, SequenceProperty& out);
[[nodiscard]] UHE_API const char* SequenceInterpName(SequenceInterp interp);
UHE_API bool SequenceInterpFromName(const std::string& name, SequenceInterp& out);

} // namespace UHE
