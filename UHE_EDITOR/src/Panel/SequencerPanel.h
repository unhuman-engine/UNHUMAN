#pragma once

#include <utility>
#include <vector>
#include "UHE/Scene/Components.h"
#include "UHE/Scene/Entity.h"

struct ImDrawList;

namespace UHE
{
/**
 * @brief The Sequencer: a timeline editor for an entity's SequencerComponent.
 *
 * The panel keeps showing the last sequencer selected, so selecting another
 * entity (to pose it, or to add a track for it) does not lose the timeline.
 * In Play it shows the running copy of the same entity and its transport
 * drives the live playback.
 *
 * Editing: scrubbing or playing previews the sequence on the edit scene. The
 * preview applies only when the playhead or a key changes, so the gizmo can
 * pose a target in between; a track's K button keys the target's current
 * value at the playhead. Everything the preview moved goes back when it ends
 * (Stop, or EndPreview, which the editor calls before Play, save and a scene
 * switch), so a preview never ends up in a saved scene.
 */
class SequencerPanel
{
public:
    /**
     * @param scene    the active scene (the running copy in Play)
     * @param selected the hierarchy's selection
     * @param editing  edit mode: preview on the scene; otherwise drive the live playback
     */
    void OnImGuiRender(bool& open, Scene* scene, Entity selected, bool editing);

    /// @brief Puts back every transform the preview moved and leaves preview.
    void EndPreview();
    [[nodiscard]] bool IsPreviewing() const { return m_Previewing; }

private:
    void ResolveContext(Scene* scene, Entity selected);
    void DrawToolbar(Scene& scene, SequencerComponent& sequencer, Entity selected, bool editing);
    void DrawTimeline(Scene& scene, SequencerComponent& sequencer, bool editing);
    /// @return true when the selected key changed
    bool DrawKeyDetails(SequencerComponent& sequencer);
    void DrawAddTrackPopup(Scene& scene, SequencerComponent& sequencer, Entity selected);

    /// @brief Moves the playhead: previews in edit mode, seeks the live playback in Play.
    void Seek(Scene& scene, SequencerComponent& sequencer, float time, bool editing);
    /// @brief Remembers each target's transform the first time the preview touches it.
    void SnapshotTargets(Scene& scene, const Sequence& sequence);
    /// @brief The current value of \p track's property on its target, or the track's value at the playhead.
    [[nodiscard]] glm::vec3 CurrentValue(Scene& scene, const SequenceTrack& track, float time) const;

    [[nodiscard]] float TimeAt(float x) const;
    [[nodiscard]] float XAt(float time) const;

    Entity m_Sequencer; ///< the timeline shown; checked every frame (Entity knows when its scene is gone)
    u64 m_SequencerUUID = 0;

    // Edit-mode preview.
    bool m_Previewing = false;
    bool m_PreviewPlaying = false;
    float m_PreviewTime = 0.0f;
    Scene* m_PreviewScene = nullptr;
    u64 m_PreviewSceneId = 0;
    std::vector<std::pair<u64, TransformComponent>> m_Saved;

    // Timeline view and selection.
    float m_PixelsPerSecond = 120.0f;
    float m_ScrollX = 0.0f;
    float m_TimelineX = 0.0f; ///< screen x of time 0 (before scrolling)
    int m_SelectedTrack = -1;
    int m_SelectedKey = -1;
    bool m_DraggingKey = false;
    bool m_FitPending = true; ///< zoom to the whole sequence on the next timeline draw
};

} // namespace UHE
