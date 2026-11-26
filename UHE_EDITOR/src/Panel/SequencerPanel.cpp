#include "Panel/SequencerPanel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "EditorTheme.h"
#include "UHE/Scene/Scene.h"
#include "imgui.h"

namespace UHE
{
namespace
{
constexpr float kRowHeight = 24.0f;
constexpr float kRulerHeight = 22.0f;
constexpr float kMaxLabelWidth = 250.0f;
constexpr float kMinPixelsPerSecond = 4.0f;
constexpr float kMaxPixelsPerSecond = 600.0f;
constexpr float kKeyRadius = 5.5f;
constexpr float kSnapStep = 1.0f / 30.0f; ///< keys snap to 30 fps frames; Alt places them freely
constexpr float kMinKeyGap = 0.001f;

float Snap(float time)
{
    return ImGui::GetIO().KeyAlt ? time : std::round(time / kSnapStep) * kSnapStep;
}

ImU32 TrackColour(SequenceProperty property, bool muted)
{
    ImVec4 colour;
    switch (property)
    {
        case SequenceProperty::Position:
            colour = ImVec4(0.93f, 0.47f, 0.33f, 1.0f);
            break;
        case SequenceProperty::Rotation:
            colour = ImVec4(0.45f, 0.82f, 0.42f, 1.0f);
            break;
        case SequenceProperty::Scale:
            colour = ImVec4(0.42f, 0.62f, 1.0f, 1.0f);
            break;
        case SequenceProperty::Event:
            colour = ImVec4(1.0f, 0.80f, 0.30f, 1.0f);
            break;
    }
    if (muted)
        colour = ImVec4(0.45f, 0.45f, 0.48f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(colour);
}

std::string EntityName(Entity entity)
{
    if (auto* tag = entity.TryGetComponent<TagComponent>())
        return tag->Tag;
    return "(missing)";
}

std::string TrackLabel(Scene& scene, const SequenceTrack& track)
{
    if (track.Property == SequenceProperty::Event)
        return "Events";
    return EntityName(scene.GetEntityWithUUID(track.Target)) + "  " + SequencePropertyName(track.Property);
}

/// The ruler's tick spacing: the first step at least 60 px apart.
float TickStep(float pixelsPerSecond)
{
    for (const float step : {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f, 60.0f})
    {
        if (step * pixelsPerSecond >= 60.0f)
            return step;
    }
    return 120.0f;
}

/// SameLine, or a new line when an item \p width wide would not fit: the toolbar wraps in a narrow dock.
void SameLineIfFits(float width)
{
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < width)
        ImGui::NewLine();
}

void DrawDiamond(ImDrawList* draw, ImVec2 centre, float radius, ImU32 fill, bool selected)
{
    const ImVec2 points[4] = {ImVec2(centre.x, centre.y - radius), ImVec2(centre.x + radius, centre.y),
                              ImVec2(centre.x, centre.y + radius), ImVec2(centre.x - radius, centre.y)};
    draw->AddConvexPolyFilled(points, 4, fill);
    draw->AddPolyline(points, 4, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 160), ImDrawFlags_Closed,
                      selected ? 2.0f : 1.0f);
}
} // namespace

float SequencerPanel::TimeAt(float x) const
{
    return (x - m_TimelineX + m_ScrollX) / m_PixelsPerSecond;
}

float SequencerPanel::XAt(float time) const
{
    return m_TimelineX + time * m_PixelsPerSecond - m_ScrollX;
}

void SequencerPanel::ResolveContext(Scene* scene, Entity selected)
{
    if (selected && selected.HasComponent<SequencerComponent>() && selected != m_Sequencer)
    {
        EndPreview();
        m_Sequencer = selected;
        m_SequencerUUID = selected.GetUUID();
        m_SelectedTrack = m_SelectedKey = -1;
        m_PreviewTime = 0.0f;
        m_FitPending = true;
    }
    // Play and Stop swap the active scene: follow the same entity into it.
    if (scene != nullptr && m_SequencerUUID != 0 && (!m_Sequencer || m_Sequencer.GetScene() != scene))
    {
        Entity same = scene->GetEntityWithUUID(m_SequencerUUID);
        if (same && same.HasComponent<SequencerComponent>())
            m_Sequencer = same;
    }
}

void SequencerPanel::SnapshotTargets(Scene& scene, const Sequence& sequence)
{
    for (const SequenceTrack& track : sequence.Tracks)
    {
        if (track.Property == SequenceProperty::Event)
            continue;
        const bool saved =
            std::any_of(m_Saved.begin(), m_Saved.end(), [&](const auto& entry) { return entry.first == track.Target; });
        if (saved)
            continue;
        Entity target = scene.GetEntityWithUUID(track.Target);
        if (auto* transform = target.TryGetComponent<TransformComponent>())
            m_Saved.emplace_back(track.Target, *transform);
    }
}

void SequencerPanel::EndPreview()
{
    if (m_Previewing && m_PreviewScene != nullptr && Scene::IsAlive(m_PreviewSceneId))
    {
        for (const auto& [uuid, saved] : m_Saved)
        {
            Entity target = m_PreviewScene->GetEntityWithUUID(uuid);
            if (auto* transform = target.TryGetComponent<TransformComponent>())
                *transform = saved;
        }
    }
    m_Previewing = false;
    m_PreviewPlaying = false;
    m_PreviewScene = nullptr;
    m_PreviewSceneId = 0;
    m_Saved.clear();
}

void SequencerPanel::Seek(Scene& scene, SequencerComponent& sequencer, float time, bool editing)
{
    time = std::clamp(time, 0.0f, sequencer.Data.Duration);
    if (editing)
    {
        if (!m_Previewing)
        {
            m_Previewing = true;
            m_PreviewScene = &scene;
            m_PreviewSceneId = scene.GetInstanceId();
        }
        m_PreviewTime = time;
        SnapshotTargets(scene, sequencer.Data);
    }
    else
    {
        sequencer.Time = time;
    }
    scene.ApplySequence(sequencer.Data, time);
}

glm::vec3 SequencerPanel::CurrentValue(Scene& scene, const SequenceTrack& track, float time) const
{
    Entity target = scene.GetEntityWithUUID(track.Target);
    const glm::vec3 sampled = Sequence::Sample(track, time, glm::vec3(0.0f));
    const auto* transform = target.TryGetComponent<TransformComponent>();
    if (transform == nullptr)
        return sampled;
    switch (track.Property)
    {
        case SequenceProperty::Position:
            return transform->Translation;
        case SequenceProperty::Rotation:
            return transform->Rotation;
        case SequenceProperty::Scale:
            return transform->Scale;
        case SequenceProperty::Event:
            break;
    }
    return sampled;
}

void SequencerPanel::OnImGuiRender(bool& open, Scene* scene, Entity selected, bool editing)
{
    if (!ImGui::Begin("Sequencer", &open))
    {
        ImGui::End();
        return;
    }
    // The scene the preview moved is gone (closed without EndPreview): nothing to put back.
    if (m_Previewing && !Scene::IsAlive(m_PreviewSceneId))
        EndPreview();
    if (!editing && m_Previewing)
        EndPreview();
    if (scene == nullptr)
    {
        ImGui::TextDisabled("No scene is open.");
        ImGui::End();
        return;
    }
    ResolveContext(scene, selected);

    auto* sequencer = m_Sequencer.TryGetComponent<SequencerComponent>();
    if (sequencer == nullptr)
    {
        ImGui::TextDisabled("Select an entity with a Sequencer to edit its timeline.");
        if (selected && editing)
        {
            const std::string label = "Add a Sequencer to '" + EntityName(selected) + "'";
            if (ImGui::Button(label.c_str()))
                selected.AddComponent<SequencerComponent>();
        }
        ImGui::End();
        return;
    }

    // Edit-mode preview playback: the panel's own clock, no events.
    if (editing && m_Previewing && m_PreviewPlaying)
    {
        bool playing = true;
        float time = m_PreviewTime;
        sequencer->Data.Advance(time, playing, ImGui::GetIO().DeltaTime * sequencer->Speed, nullptr);
        m_PreviewPlaying = playing;
        Seek(*scene, *sequencer, time, true);
    }

    DrawToolbar(*scene, *sequencer, selected, editing);
    DrawTimeline(*scene, *sequencer, editing);
    if (DrawKeyDetails(*sequencer) && (m_Previewing || !editing))
        Seek(*scene, *sequencer, editing ? m_PreviewTime : sequencer->Time, editing);
    ImGui::End();
}

void SequencerPanel::DrawToolbar(Scene& scene, SequencerComponent& sequencer, Entity selected, bool editing)
{
    Sequence& data = sequencer.Data;
    const float shownTime = editing ? m_PreviewTime : sequencer.Time;
    const bool playing = editing ? m_PreviewPlaying : sequencer.Playing;

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(EditorTheme::Accent(), "%s", EntityName(m_Sequencer).c_str());
    SameLineIfFits(50.0f);
    if (ImGui::Button(playing ? "Pause" : "Play"))
    {
        const float start = !data.Loop && shownTime >= data.Duration ? 0.0f : shownTime;
        if (editing)
        {
            if (!m_PreviewPlaying)
                Seek(scene, sequencer, start, true);
            m_PreviewPlaying = !m_PreviewPlaying;
        }
        else
        {
            if (!sequencer.Playing)
                sequencer.Time = start;
            sequencer.Playing = !sequencer.Playing;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
    {
        if (editing)
        {
            m_PreviewTime = 0.0f;
            EndPreview();
        }
        else
        {
            sequencer.Playing = false;
            Seek(scene, sequencer, 0.0f, false);
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip(editing ? "Back to 0 and end the preview: everything it moved goes back."
                                  : "Stop the running sequence and pose its start.");
    SameLineIfFits(120.0f);
    ImGui::Text("%6.2f / %.2f s", shownTime, data.Duration);

    SameLineIfFits(130.0f);
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("Length", &data.Duration, 0.05f, 0.1f, 3600.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
    SameLineIfFits(70.0f);
    ImGui::Checkbox("Loop", &data.Loop);
    SameLineIfFits(120.0f);
    ImGui::Checkbox("Play on Start", &sequencer.PlayOnStart);
    SameLineIfFits(120.0f);
    ImGui::SetNextItemWidth(70.0f);
    ImGui::DragFloat("Speed", &sequencer.Speed, 0.01f, -4.0f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    SameLineIfFits(160.0f);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("Zoom", &m_PixelsPerSecond, kMinPixelsPerSecond, kMaxPixelsPerSecond, "%.0f px/s",
                       ImGuiSliderFlags_Logarithmic);
    SameLineIfFits(40.0f);
    if (ImGui::Button("Fit"))
        m_FitPending = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Zoom to show the whole sequence.");
    SameLineIfFits(70.0f);
    if (ImGui::Button("+ Track"))
        ImGui::OpenPopup("##SequencerAddTrack");
    DrawAddTrackPopup(scene, sequencer, selected);

    if (!editing)
        ImGui::TextDisabled("Playing: edits made here are discarded on Stop.");
}

void SequencerPanel::DrawAddTrackPopup(Scene& scene, SequencerComponent& sequencer, Entity selected)
{
    if (!ImGui::BeginPopup("##SequencerAddTrack"))
        return;
    Entity target = selected ? selected : m_Sequencer;
    const float playhead = m_Previewing ? m_PreviewTime : sequencer.Time;
    if (target.HasComponent<TransformComponent>())
    {
        ImGui::TextDisabled("%s", EntityName(target).c_str());
        for (const SequenceProperty property :
             {SequenceProperty::Position, SequenceProperty::Rotation, SequenceProperty::Scale})
        {
            if (ImGui::MenuItem(SequencePropertyName(property)))
            {
                SequenceTrack track;
                track.Target = target.GetUUID();
                track.Property = property;
                // The first key holds the target's pose now, at the playhead.
                Sequence::SetKey(track, playhead, CurrentValue(scene, track, playhead));
                sequencer.Data.Tracks.push_back(std::move(track));
                m_SelectedTrack = static_cast<int>(sequencer.Data.Tracks.size()) - 1;
                m_SelectedKey = 0;
            }
        }
        ImGui::Separator();
    }
    if (ImGui::MenuItem("Events"))
    {
        SequenceTrack track;
        track.Property = SequenceProperty::Event;
        sequencer.Data.Tracks.push_back(std::move(track));
        m_SelectedTrack = static_cast<int>(sequencer.Data.Tracks.size()) - 1;
        m_SelectedKey = -1;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Named keys; the sequencer entity's script gets OnSequenceEvent(self, name).");
    ImGui::EndPopup();
}

void SequencerPanel::DrawTimeline(Scene& scene, SequencerComponent& sequencer, bool editing)
{
    Sequence& data = sequencer.Data;
    const float detailsHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f + 6.0f;
    if (!ImGui::BeginChild("##SequencerBody", ImVec2(0.0f, -detailsHeight), ImGuiChildFlags_Borders))
    {
        ImGui::EndChild();
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 200.0f);
    const float right = origin.x + width;
    const float labelWidth = std::min(kMaxLabelWidth, std::max(140.0f, width * 0.35f));
    m_TimelineX = origin.x + labelWidth;
    if (m_FitPending)
    {
        m_PixelsPerSecond = std::clamp((right - m_TimelineX - 16.0f) / std::max(data.Duration, 0.1f),
                                       kMinPixelsPerSecond, kMaxPixelsPerSecond);
        m_ScrollX = 0.0f;
        m_FitPending = false;
    }
    const float playhead = editing ? m_PreviewTime : sequencer.Time;
    const float rowsBottom = origin.y + kRulerHeight + static_cast<float>(data.Tracks.size()) * kRowHeight;

    // Ctrl+wheel zooms around the mouse; Shift+wheel scrolls through time.
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f && (io.KeyCtrl || io.KeyShift))
    {
        if (io.KeyCtrl)
        {
            const float anchor = TimeAt(io.MousePos.x);
            m_PixelsPerSecond = std::clamp(m_PixelsPerSecond * std::pow(1.15f, io.MouseWheel), kMinPixelsPerSecond,
                                           kMaxPixelsPerSecond);
            m_ScrollX = std::max(0.0f, anchor * m_PixelsPerSecond - (io.MousePos.x - m_TimelineX));
        }
        else
        {
            m_ScrollX = std::max(0.0f, m_ScrollX - io.MouseWheel * 40.0f);
        }
    }

    // ── Ruler: click or drag to move the playhead ──
    ImGui::SetCursorScreenPos(ImVec2(m_TimelineX, origin.y));
    ImGui::InvisibleButton("##Ruler", ImVec2(right - m_TimelineX, kRulerHeight));
    if (ImGui::IsItemActive())
        Seek(scene, sequencer, Snap(TimeAt(io.MousePos.x)), editing);
    draw->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(right, origin.y + kRulerHeight),
                        ImGui::GetColorU32(ImGuiCol_FrameBg));
    draw->PushClipRect(ImVec2(m_TimelineX, origin.y), ImVec2(right, rowsBottom + 2.0f), true);
    const float step = TickStep(m_PixelsPerSecond);
    const int firstTick = std::max(0, static_cast<int>(std::floor(TimeAt(m_TimelineX) / step)));
    for (int tick = firstTick; XAt(static_cast<float>(tick) * step) < right; ++tick)
    {
        const float t = static_cast<float>(tick) * step;
        const float x = XAt(t);
        draw->AddLine(ImVec2(x, origin.y + kRulerHeight - 7.0f), ImVec2(x, origin.y + kRulerHeight),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled));
        draw->AddLine(ImVec2(x, origin.y + kRulerHeight), ImVec2(x, rowsBottom), IM_COL32(255, 255, 255, 14));
        char label[16];
        std::snprintf(label, sizeof(label), step < 1.0f ? "%.2f" : "%.0f", t);
        draw->AddText(ImVec2(x + 3.0f, origin.y + 2.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    }
    // Past the end: shaded.
    const float endX = XAt(data.Duration);
    if (endX < right)
        draw->AddRectFilled(ImVec2(std::max(endX, m_TimelineX), origin.y), ImVec2(right, rowsBottom),
                            IM_COL32(0, 0, 0, 70));
    draw->PopClipRect();

    // ── Tracks ──
    int removeTrack = -1;
    bool keysChanged = false;
    for (int i = 0; i < static_cast<int>(data.Tracks.size()); ++i)
    {
        SequenceTrack& track = data.Tracks[i];
        const float y = origin.y + kRulerHeight + static_cast<float>(i) * kRowHeight;
        ImGui::PushID(i);
        if (i % 2 == 1)
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(right, y + kRowHeight), IM_COL32(255, 255, 255, 8));
        if (i == m_SelectedTrack)
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(m_TimelineX, y + kRowHeight),
                                ImGui::GetColorU32(ImGuiCol_Header));

        // Label side: key, mute, delete, name.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 4.0f, y + (kRowHeight - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::SmallButton("K"))
        {
            if (track.Property == SequenceProperty::Event)
            {
                const std::size_t at = Sequence::SetKey(track, playhead, glm::vec3(0.0f));
                if (track.Keys[at].Event.empty())
                    track.Keys[at].Event = "Event";
                m_SelectedKey = static_cast<int>(at);
            }
            else
            {
                m_SelectedKey =
                    static_cast<int>(Sequence::SetKey(track, playhead, CurrentValue(scene, track, playhead)));
            }
            m_SelectedTrack = i;
            keysChanged = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(track.Property == SequenceProperty::Event
                                  ? "Add an event key at the playhead."
                                  : "Key the target's current value at the playhead (pose it with the gizmo first).");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text,
                              track.Muted ? EditorTheme::Warning() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (ImGui::SmallButton(track.Muted ? "M" : "m"))
        {
            track.Muted = !track.Muted;
            keysChanged = true;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(track.Muted ? "Muted: click to play this track again." : "Mute this track.");
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            removeTrack = i;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Delete this track.");
        ImGui::SameLine();
        const std::string label = TrackLabel(scene, track);
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(m_TimelineX - 4.0f, y + kRowHeight), true);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TrackColour(track.Property, track.Muted)), "%s",
                           label.c_str());
        ImGui::PopClipRect();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", label.c_str());

        // Timeline side: keys.
        ImGui::SetCursorScreenPos(ImVec2(m_TimelineX, y));
        ImGui::InvisibleButton("##Row", ImVec2(right - m_TimelineX, kRowHeight),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool rowHovered = ImGui::IsItemHovered();
        const float centreY = y + kRowHeight * 0.5f;
        int hoveredKey = -1;
        if (rowHovered)
        {
            float best = kKeyRadius + 3.0f;
            for (int k = 0; k < static_cast<int>(track.Keys.size()); ++k)
            {
                const float distance = std::fabs(XAt(track.Keys[k].Time) - io.MousePos.x);
                if (distance <= best)
                {
                    best = distance;
                    hoveredKey = k;
                }
            }
        }
        if (rowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_SelectedTrack = i;
            m_SelectedKey = hoveredKey;
            m_DraggingKey = hoveredKey >= 0;
            if (hoveredKey < 0)
                Seek(scene, sequencer, Snap(TimeAt(io.MousePos.x)), editing);
        }
        if (rowHovered && hoveredKey < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            // Double-click on empty track: a key there, holding the value the track already has.
            const float time = std::clamp(Snap(TimeAt(io.MousePos.x)), 0.0f, data.Duration);
            const glm::vec3 value = track.Property == SequenceProperty::Event
                                        ? glm::vec3(0.0f)
                                        : Sequence::Sample(track, time, CurrentValue(scene, track, time));
            const std::size_t at = Sequence::SetKey(track, time, value);
            if (track.Property == SequenceProperty::Event && track.Keys[at].Event.empty())
                track.Keys[at].Event = "Event";
            m_SelectedTrack = i;
            m_SelectedKey = static_cast<int>(at);
            keysChanged = true;
        }
        if (rowHovered && hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_SelectedTrack = i;
            m_SelectedKey = hoveredKey;
            ImGui::OpenPopup("##SequencerKey");
        }

        draw->PushClipRect(ImVec2(m_TimelineX, y), ImVec2(right, y + kRowHeight), true);
        for (int k = 0; k < static_cast<int>(track.Keys.size()); ++k)
        {
            const bool isSelected = i == m_SelectedTrack && k == m_SelectedKey;
            DrawDiamond(draw, ImVec2(XAt(track.Keys[k].Time), centreY), kKeyRadius,
                        TrackColour(track.Property, track.Muted), isSelected);
        }
        draw->PopClipRect();

        if (ImGui::BeginPopup("##SequencerKey"))
        {
            if (m_SelectedTrack == i && m_SelectedKey >= 0 && m_SelectedKey < static_cast<int>(track.Keys.size()))
            {
                SequenceKey& key = track.Keys[m_SelectedKey];
                if (track.Property != SequenceProperty::Event)
                {
                    for (const SequenceInterp interp :
                         {SequenceInterp::Step, SequenceInterp::Linear, SequenceInterp::Smooth})
                    {
                        if (ImGui::MenuItem(SequenceInterpName(interp), nullptr, key.Interp == interp))
                        {
                            key.Interp = interp;
                            keysChanged = true;
                        }
                    }
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Delete Key", "Del"))
                {
                    track.Keys.erase(track.Keys.begin() + m_SelectedKey);
                    m_SelectedKey = -1;
                    keysChanged = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    // Dragging a key: stays between its neighbours, so the keys stay sorted.
    if (m_DraggingKey)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || m_SelectedTrack < 0 ||
            m_SelectedTrack >= static_cast<int>(data.Tracks.size()) || m_SelectedKey < 0 ||
            m_SelectedKey >= static_cast<int>(data.Tracks[m_SelectedTrack].Keys.size()))
        {
            m_DraggingKey = false;
        }
        else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
        {
            std::vector<SequenceKey>& keys = data.Tracks[m_SelectedTrack].Keys;
            const float lower = m_SelectedKey > 0 ? keys[m_SelectedKey - 1].Time + kMinKeyGap : 0.0f;
            const float upper = m_SelectedKey + 1 < static_cast<int>(keys.size())
                                    ? keys[m_SelectedKey + 1].Time - kMinKeyGap
                                    : data.Duration;
            keys[m_SelectedKey].Time = std::clamp(Snap(TimeAt(io.MousePos.x)), lower, std::max(lower, upper));
            keysChanged = true;
        }
    }

    // Del removes the selected key.
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete) && m_SelectedTrack >= 0 &&
        m_SelectedTrack < static_cast<int>(data.Tracks.size()) && m_SelectedKey >= 0 &&
        m_SelectedKey < static_cast<int>(data.Tracks[m_SelectedTrack].Keys.size()))
    {
        std::vector<SequenceKey>& keys = data.Tracks[m_SelectedTrack].Keys;
        keys.erase(keys.begin() + m_SelectedKey);
        m_SelectedKey = -1;
        keysChanged = true;
    }

    if (removeTrack >= 0)
    {
        data.Tracks.erase(data.Tracks.begin() + removeTrack);
        m_SelectedTrack = m_SelectedKey = -1;
        keysChanged = true;
    }

    // The playhead, over the ruler and every row.
    const float x = XAt(playhead);
    if (x >= m_TimelineX && x <= right)
    {
        const ImU32 colour = ImGui::ColorConvertFloat4ToU32(EditorTheme::Accent());
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, std::max(rowsBottom, origin.y + kRulerHeight)), colour, 2.0f);
        draw->AddTriangleFilled(ImVec2(x - 6.0f, origin.y), ImVec2(x + 6.0f, origin.y), ImVec2(x, origin.y + 8.0f),
                                colour);
    }

    if (data.Tracks.empty())
    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, origin.y + kRulerHeight + 8.0f));
        ImGui::TextDisabled("No tracks. + Track adds one for the selected entity (or this one).");
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, std::max(rowsBottom, origin.y + kRulerHeight + 40.0f)));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));

    // A changed key shows at once wherever the sequence is being shown.
    if (keysChanged && (m_Previewing || !editing))
        Seek(scene, sequencer, playhead, editing);
    ImGui::EndChild();
}

bool SequencerPanel::DrawKeyDetails(SequencerComponent& sequencer)
{
    Sequence& data = sequencer.Data;
    if (m_SelectedTrack < 0 || m_SelectedTrack >= static_cast<int>(data.Tracks.size()) || m_SelectedKey < 0 ||
        m_SelectedKey >= static_cast<int>(data.Tracks[m_SelectedTrack].Keys.size()))
    {
        ImGui::TextDisabled("Click a key to edit it. (?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Double-click a track to add a key; drag keys to move them; right-click a key for its "
                              "curve; Del deletes.\nKeys snap to 1/30 s (hold Alt to place freely).\n"
                              "Ctrl+wheel zooms, Shift+wheel scrolls. K keys the target's current pose.");
        return false;
    }
    SequenceTrack& track = data.Tracks[m_SelectedTrack];
    std::vector<SequenceKey>& keys = track.Keys;
    SequenceKey& key = keys[m_SelectedKey];

    const float lower = m_SelectedKey > 0 ? keys[m_SelectedKey - 1].Time + kMinKeyGap : 0.0f;
    const float upper =
        m_SelectedKey + 1 < static_cast<int>(keys.size()) ? keys[m_SelectedKey + 1].Time - kMinKeyGap : data.Duration;
    bool changed = false;
    ImGui::SetNextItemWidth(90.0f);
    changed |= ImGui::DragFloat("Time", &key.Time, 0.01f, lower, std::max(lower, upper), "%.3f s",
                                ImGuiSliderFlags_AlwaysClamp);
    SameLineIfFits(track.Property == SequenceProperty::Event ? 280.0f : 340.0f);
    if (track.Property == SequenceProperty::Event)
    {
        char name[128];
        std::snprintf(name, sizeof(name), "%s", key.Event.c_str());
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::InputText("Event", name, sizeof(name)))
            key.Event = name;
        return changed;
    }
    ImGui::SetNextItemWidth(260.0f);
    if (track.Property == SequenceProperty::Rotation)
    {
        // Stored in radians like the transform; edited in degrees like the inspector.
        glm::vec3 degrees = glm::degrees(key.Value);
        if (ImGui::DragFloat3("Value (deg)", &degrees.x, 0.5f))
        {
            key.Value = glm::radians(degrees);
            changed = true;
        }
    }
    else
    {
        changed |= ImGui::DragFloat3("Value", &key.Value.x, 0.05f);
    }
    SameLineIfFits(140.0f);
    int interp = static_cast<int>(key.Interp);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::Combo("Curve", &interp, "Step\0Linear\0Smooth\0"))
    {
        key.Interp = static_cast<SequenceInterp>(interp);
        changed = true;
    }
    return changed;
}

} // namespace UHE
