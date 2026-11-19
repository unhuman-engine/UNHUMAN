#include "Panel/AnimationGraphPanel.h"

#include "EditorTheme.h"
#include "UHE/Renderer3D/Animator.h"
#include "UHE/Renderer3D/AnimationStateMachine.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace UHE
{
    namespace
    {
        constexpr float kNodeWidth = 190.0f;
        constexpr float kNodeHeight = 68.0f;
        constexpr float kPortRadius = 5.0f;
        constexpr float kHeaderHeight = 24.0f;

        const char* kConditionNames[] = {"Greater", "Less", "Is True", "Is False", "Trigger"};

        ImVec4 WithAlpha(const ImVec4& color, float alpha)
        {
            return ImVec4(color.x, color.y, color.z, alpha);
        }

        std::vector<std::string> ClipNames(const AnimatorComponent& anim)
        {
            std::vector<std::string> names;
            if (!anim.Animator)
                return names;

            const Ref<RD3d::Model> model = anim.Animator->GetModel();
            if (!model)
                return names;

            for (const auto& clip : model->GetAnimations())
                names.push_back(clip.Name);
            return names;
        }

        std::string UniqueStateName(const AnimationGraph& graph)
        {
            int suffix = static_cast<int>(graph.States.size()) + 1;
            std::string name = "State " + std::to_string(suffix);
            auto taken = [&graph](const std::string& candidate)
            {
                return std::any_of(graph.States.begin(), graph.States.end(),
                                   [&candidate](const AnimationGraphState& state) { return state.Name == candidate; });
            };
            while (taken(name))
                name = "State " + std::to_string(++suffix);
            return name;
        }

        ImVec2 BezierPoint(const ImVec2& p0, const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, float t)
        {
            const float u = 1.0f - t;
            const float uu = u * u;
            const float tt = t * t;
            const float uuu = uu * u;
            const float ttt = tt * t;

            ImVec2 point;
            point.x = uuu * p0.x + 3.0f * uu * t * p1.x + 3.0f * u * tt * p2.x + ttt * p3.x;
            point.y = uuu * p0.y + 3.0f * uu * t * p1.y + 3.0f * u * tt * p2.y + ttt * p3.y;
            return point;
        }

        float DistanceToSegment(const ImVec2& point, const ImVec2& a, const ImVec2& b)
        {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float lengthSq = dx * dx + dy * dy;
            if (lengthSq <= 0.0001f)
                return std::sqrt((point.x - a.x) * (point.x - a.x) + (point.y - a.y) * (point.y - a.y));

            float t = ((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSq;
            t = std::clamp(t, 0.0f, 1.0f);
            const ImVec2 closest(a.x + t * dx, a.y + t * dy);
            return std::sqrt((point.x - closest.x) * (point.x - closest.x) +
                             (point.y - closest.y) * (point.y - closest.y));
        }

        bool ClipCombo(const char* label, std::string& clip, const std::vector<std::string>& clips)
        {
            bool changed = false;
            if (ImGui::BeginCombo(label, clip.empty() ? "(none)" : clip.c_str()))
            {
                if (ImGui::Selectable("(none)", clip.empty()))
                {
                    clip.clear();
                    changed = true;
                }
                for (const std::string& name : clips)
                {
                    if (ImGui::Selectable(name.c_str(), clip == name))
                    {
                        clip = name;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            return changed;
        }
    } // namespace

    void AnimationGraphPanel::OnImGuiRender(bool& open, Entity selected)
    {
        if (!ImGui::Begin("Animation Graph", &open))
        {
            ImGui::End();
            return;
        }

        const u64 entityID = selected ? static_cast<u64>(selected.GetUUID()) : 0;
        if (entityID != m_ContextEntity)
        {
            m_ContextEntity = entityID;
            m_SelectedState = -1;
            m_SelectedTransition = -1;
            m_LinkFrom = -1;
            m_DragState = -1;
            m_FramePending = true;
        }

        if (!selected)
        {
            ImGui::TextDisabled("Select an entity with an Animator 3D component.");
            ImGui::End();
            return;
        }

        if (!selected.HasComponent<AnimatorComponent>())
        {
            ImGui::TextDisabled("'%s' has no Animator 3D component.",
                                selected.HasComponent<TagComponent>()
                                    ? selected.GetComponent<TagComponent>().Tag.c_str()
                                    : "Entity");
            ImGui::End();
            return;
        }

        AnimatorComponent& anim = selected.GetComponent<AnimatorComponent>();

        if (!anim.Animator)
        {
            ImGui::TextDisabled("The animator is not initialized.");
            if (selected.HasComponent<Model3DComponent>())
            {
                Model3DComponent& model = selected.GetComponent<Model3DComponent>();
                if (model.IsLoaded && model.ModelData)
                {
                    if (ImGui::Button("Initialize Animator"))
                        anim.Animator = CreateRef<RD3d::Animator>(model.ModelData);
                }
                else
                {
                    ImGui::TextDisabled("Load the model first.");
                }
            }
            ImGui::End();
            return;
        }

        DrawToolbar(anim);
        ImGui::Separator();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const bool sideBySide = avail.x >= 560.0f;
        float canvasWidth = avail.x;
        float canvasHeight = avail.y;
        if (sideBySide)
        {
            const float inspectorWidth = std::min(330.0f, avail.x * 0.45f);
            canvasWidth = std::max(avail.x - inspectorWidth - 8.0f, 80.0f);
        }
        else
        {
            // Narrow dock: the inspector moves under the canvas instead of
            // squeezing both into unusable columns.
            canvasHeight = std::max(avail.y * 0.58f, avail.y - 220.0f);
        }

        ImGui::BeginChild("##graphCanvas", ImVec2(canvasWidth, canvasHeight), true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawCanvas(anim);
        ImGui::EndChild();

        if (sideBySide)
            ImGui::SameLine();

        ImGui::BeginChild("##graphInspector", ImVec2(0.0f, 0.0f), false);
        DrawInspector(anim);
        ImGui::EndChild();

        ImGui::End();
    }

    void AnimationGraphPanel::DrawToolbar(AnimatorComponent& anim)
    {
        AnimationGraph& graph = anim.Graph;

        ImGui::Checkbox("Enabled", &graph.Enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The graph drives the clip, loop mode and cross-fades while enabled.");

        ImGui::SameLine();
        if (ImGui::Button("Add State"))
            AddState(anim);
        ImGui::SameLine();
        if (ImGui::Button("Add Parameter"))
        {
            std::snprintf(m_ParameterNameBuffer, sizeof(m_ParameterNameBuffer), "param%d",
                          static_cast<int>(graph.Parameters.size()) + 1);
            graph.Parameters.push_back({m_ParameterNameBuffer, 0.0f});
        }
        ImGui::SameLine();
        if (ImGui::Button("Frame All"))
            FrameAll(anim);
        ImGui::SameLine();
        if (ImGui::Button("Reset Runtime"))
        {
            anim.GraphRuntime.reset();
            anim.GraphRuntimeHash = 0;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restart from the entry state without stopping the scene.");

        ImGui::SameLine();
        if (graph.Enabled && anim.GraphRuntime)
        {
            const std::string& stateName = anim.GraphRuntime->GetCurrentState();
            ImGui::TextDisabled("  %zu states · running '%s'", graph.States.size(),
                                stateName.empty() ? "(none)" : stateName.c_str());
        }
        else
            ImGui::TextDisabled("  %zu states · %zu links", graph.States.size(), graph.Transitions.size());
    }

    void AnimationGraphPanel::DrawCanvas(AnimatorComponent& anim)
    {
        AnimationGraph& graph = anim.Graph;
        m_CanvasMin = ImGui::GetCursorScreenPos();
        m_CanvasSize = ImGui::GetContentRegionAvail();
        if (m_CanvasSize.x < 20.0f || m_CanvasSize.y < 20.0f)
            return;

        if (m_FramePending)
        {
            FrameAll(anim);
            m_FramePending = false;
        }

        ImGui::InvisibleButton("##graphCanvasInput", m_CanvasSize,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        ImGuiIO& io = ImGui::GetIO();
        ImDrawList* draw = ImGui::GetWindowDrawList();

        const ImVec2 canvasMax(m_CanvasMin.x + m_CanvasSize.x, m_CanvasMin.y + m_CanvasSize.y);
        draw->PushClipRect(m_CanvasMin, canvasMax, true);
        draw->AddRectFilled(m_CanvasMin, canvasMax, ImGui::GetColorU32(ImGuiCol_ChildBg));

        if (graph.States.empty())
        {
            const char* hint = "Right-click or Add State to start";
            const ImVec2 hintSize = ImGui::CalcTextSize(hint);
            draw->AddText(ImVec2(m_CanvasMin.x + (m_CanvasSize.x - hintSize.x) * 0.5f,
                                 m_CanvasMin.y + (m_CanvasSize.y - hintSize.y) * 0.5f),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
        }

        // Dot grid: a static mesh of where world space is, independent of how
        // many states exist.
        const float gridStep = 32.0f * m_Zoom;
        if (gridStep >= 10.0f)
        {
            const ImU32 gridColor = ImGui::GetColorU32(ImGuiCol_Separator, 0.22f);
            float startX = std::fmod(m_Pan.x, gridStep);
            if (startX < 0.0f)
                startX += gridStep;
            for (float x = startX; x < m_CanvasSize.x; x += gridStep)
                draw->AddLine(ImVec2(m_CanvasMin.x + x, m_CanvasMin.y),
                              ImVec2(m_CanvasMin.x + x, canvasMax.y), gridColor);
            float startY = std::fmod(m_Pan.y, gridStep);
            if (startY < 0.0f)
                startY += gridStep;
            for (float y = startY; y < m_CanvasSize.y; y += gridStep)
                draw->AddLine(ImVec2(m_CanvasMin.x, m_CanvasMin.y + y),
                              ImVec2(canvasMax.x, m_CanvasMin.y + y), gridColor);
        }

        // ── view transform ──
        if (hovered && io.MouseWheel != 0.0f)
        {
            const glm::vec2 before = ScreenToWorld(io.MousePos);
            m_Zoom = std::clamp(m_Zoom * std::pow(1.15f, io.MouseWheel), 0.35f, 2.5f);
            m_Pan = glm::vec2(io.MousePos.x - m_CanvasMin.x, io.MousePos.y - m_CanvasMin.y) - before * m_Zoom;
        }

        if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                        (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left))))
        {
            m_Pan += glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
        }

        const glm::vec2 mouseWorld = ScreenToWorld(io.MousePos);
        const int hoveredNode = NodeAt(graph, mouseWorld);

        bool overOutputPort = false;
        if (hoveredNode >= 0)
        {
            const AnimationGraphState& state = graph.States[static_cast<std::size_t>(hoveredNode)];
            const ImVec2 nodeOrigin = WorldToScreen(state.EditorPosition);
            const ImVec2 port(nodeOrigin.x + kNodeWidth * m_Zoom, nodeOrigin.y + kNodeHeight * 0.5f * m_Zoom);
            const float dx = io.MousePos.x - port.x;
            const float dy = io.MousePos.y - port.y;
            overOutputPort = (dx * dx + dy * dy) <= 10.0f * 10.0f;
        }

        // ── interaction ──
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (overOutputPort && hoveredNode >= 0)
            {
                m_LinkFrom = hoveredNode;
            }
            else if (hoveredNode >= 0)
            {
                m_SelectedState = hoveredNode;
                m_SelectedTransition = -1;
                m_DragState = hoveredNode;
                m_DragOffset = mouseWorld - graph.States[static_cast<std::size_t>(hoveredNode)].EditorPosition;
            }
            else
            {
                m_SelectedState = -1;
                m_SelectedTransition = -1;
            }
        }

        if (m_DragState >= 0)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && m_DragState < static_cast<int>(graph.States.size()))
                graph.States[static_cast<std::size_t>(m_DragState)].EditorPosition = mouseWorld - m_DragOffset;
            else
                m_DragState = -1;
        }

        if (m_LinkFrom >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            int target = hoveredNode;
            if (target >= 0 && target != m_LinkFrom)
                AddTransition(graph, m_LinkFrom, target);
            m_LinkFrom = -1;
        }

        // A click on empty canvas selects the link under the cursor.
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && hoveredNode < 0 && m_DragState < 0 &&
            m_LinkFrom < 0)
        {
            const int transition = TransitionAt(graph, io.MousePos);
            if (transition >= 0)
            {
                m_SelectedTransition = transition;
                m_SelectedState = -1;
            }
        }

        // ── links (under nodes) ──
        for (int i = 0; i < static_cast<int>(graph.Transitions.size()); ++i)
        {
            const AnimationGraphTransition& transition = graph.Transitions[static_cast<std::size_t>(i)];
            if (transition.From < 0 || transition.From >= static_cast<int>(graph.States.size()) ||
                transition.To < 0 || transition.To >= static_cast<int>(graph.States.size()))
                continue;

            const ImVec2 fromOrigin = WorldToScreen(graph.States[static_cast<std::size_t>(transition.From)].EditorPosition);
            const ImVec2 from(fromOrigin.x + kNodeWidth * m_Zoom, fromOrigin.y + kNodeHeight * 0.5f * m_Zoom);
            const ImVec2 toOrigin = WorldToScreen(graph.States[static_cast<std::size_t>(transition.To)].EditorPosition);
            const ImVec2 to(toOrigin.x, toOrigin.y + kNodeHeight * 0.5f * m_Zoom);
            const float bend = std::max(40.0f * m_Zoom, std::abs(to.x - from.x) * 0.5f);
            const ImVec2 c1(from.x + bend, from.y);
            const ImVec2 c2(to.x - bend, to.y);

            const bool selected = i == m_SelectedTransition;
            const ImU32 color = selected ? ImGui::GetColorU32(EditorTheme::Warning())
                                         : ImGui::GetColorU32(EditorTheme::AccentMuted());
            draw->AddBezierCubic(from, c1, c2, to, color, selected ? 3.0f : 1.8f);

            const ImVec2 arrow = BezierPoint(from, c1, c2, to, 0.96f);
            const ImVec2 normal = ImVec2(-(to.y - c2.y), to.x - c2.x);
            const float normalLength = std::max(std::sqrt(normal.x * normal.x + normal.y * normal.y), 0.001f);
            draw->AddTriangleFilled(to, ImVec2(arrow.x + normal.x / normalLength * 4.0f,
                                               arrow.y + normal.y / normalLength * 4.0f),
                                    ImVec2(arrow.x - normal.x / normalLength * 4.0f,
                                           arrow.y - normal.y / normalLength * 4.0f),
                                    color);

            if (transition.HasExitTime || !transition.Parameter.empty())
            {
                char label[96];
                if (!transition.Parameter.empty())
                    std::snprintf(label, sizeof(label), "%s %s %.2f%s", transition.Parameter.c_str(),
                                  kConditionNames[static_cast<int>(transition.Condition)],
                                  transition.Threshold, transition.HasExitTime ? " + exit" : "");
                else
                    std::snprintf(label, sizeof(label), "exit %.2f", transition.ExitTime);

                const ImVec2 mid = BezierPoint(from, c1, c2, to, 0.5f);
                draw->AddText(ImGui::GetFont(), 11.0f * m_Zoom,
                              ImVec2(mid.x - ImGui::CalcTextSize(label).x * 0.25f, mid.y - 14.0f * m_Zoom),
                              ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
            }
        }

        if (m_LinkFrom >= 0 && m_LinkFrom < static_cast<int>(graph.States.size()))
        {
            const ImVec2 fromOrigin = WorldToScreen(graph.States[static_cast<std::size_t>(m_LinkFrom)].EditorPosition);
            const ImVec2 from(fromOrigin.x + kNodeWidth * m_Zoom, fromOrigin.y + kNodeHeight * 0.5f * m_Zoom);
            const float bend = std::max(40.0f * m_Zoom, std::abs(io.MousePos.x - from.x) * 0.5f);
            draw->AddBezierCubic(from, ImVec2(from.x + bend, from.y), ImVec2(io.MousePos.x - bend, io.MousePos.y),
                                 io.MousePos, ImGui::GetColorU32(EditorTheme::Success()), 2.0f);
        }

        // ── nodes ──
        ImFont* boldFont = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : ImGui::GetFont();
        for (int i = 0; i < static_cast<int>(graph.States.size()); ++i)
        {
            AnimationGraphState& state = graph.States[static_cast<std::size_t>(i)];
            const ImVec2 nodeMin = WorldToScreen(state.EditorPosition);
            const ImVec2 nodeMax(nodeMin.x + kNodeWidth * m_Zoom, nodeMin.y + kNodeHeight * m_Zoom);
            if (nodeMax.x < m_CanvasMin.x || nodeMin.x > canvasMax.x || nodeMax.y < m_CanvasMin.y ||
                nodeMin.y > canvasMax.y)
                continue;

            const bool selected = i == m_SelectedState;
            const ImU32 background = ImGui::GetColorU32(ImGuiCol_FrameBg, 0.97f);
            const ImU32 border = selected ? ImGui::GetColorU32(EditorTheme::Accent())
                                          : ImGui::GetColorU32(ImGuiCol_Border);
            draw->AddRectFilled(nodeMin, nodeMax, background, 6.0f * m_Zoom);
            draw->AddRectFilled(nodeMin, ImVec2(nodeMax.x, nodeMin.y + kHeaderHeight * m_Zoom),
                                ImGui::GetColorU32(WithAlpha(EditorTheme::AccentMuted(), 0.35f)), 6.0f * m_Zoom,
                                ImDrawFlags_RoundCornersTop);
            draw->AddRect(nodeMin, nodeMax, border, 6.0f * m_Zoom, 0, selected ? 2.0f : 1.0f);

            const float textSize = 14.0f * m_Zoom;
            const float smallSize = 12.0f * m_Zoom;
            draw->AddText(boldFont, textSize, ImVec2(nodeMin.x + 8.0f * m_Zoom, nodeMin.y + 4.0f * m_Zoom),
                          ImGui::GetColorU32(ImGuiCol_Text), state.Name.c_str());

            const std::string clipLabel = state.Clip.empty() ? "(no clip)" : state.Clip;
            draw->AddText(ImGui::GetFont(), smallSize, ImVec2(nodeMin.x + 8.0f * m_Zoom,
                                                             nodeMin.y + (kHeaderHeight + 7.0f) * m_Zoom),
                          state.Clip.empty() ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                                             : ImGui::GetColorU32(EditorTheme::ConsoleInfo()),
                          clipLabel.c_str());

            char speed[32];
            std::snprintf(speed, sizeof(speed), "%.2fx%s", state.Speed, state.Loop ? "" : " once");
            draw->AddText(ImGui::GetFont(), smallSize,
                          ImVec2(nodeMin.x + 8.0f * m_Zoom, nodeMin.y + (kHeaderHeight + 23.0f) * m_Zoom),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), speed);

            if (i == graph.EntryState)
                draw->AddText(ImGui::GetFont(), smallSize,
                              ImVec2(nodeMax.x - 38.0f * m_Zoom, nodeMin.y + 4.0f * m_Zoom),
                              ImGui::GetColorU32(EditorTheme::Success()), "entry");

            // Ports: input on the left edge, output on the right edge.
            draw->AddCircleFilled(ImVec2(nodeMin.x, nodeMin.y + kNodeHeight * 0.5f * m_Zoom), kPortRadius * m_Zoom,
                                  ImGui::GetColorU32(EditorTheme::ConsoleInfo()));
            draw->AddCircleFilled(ImVec2(nodeMax.x, nodeMin.y + kNodeHeight * 0.5f * m_Zoom), kPortRadius * m_Zoom,
                                  m_LinkFrom == i ? ImGui::GetColorU32(EditorTheme::Success())
                                                  : ImGui::GetColorU32(EditorTheme::Accent()));
        }

        // ── canvas context menu ──
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##graphCanvasMenu");
        if (ImGui::BeginPopup("##graphCanvasMenu"))
        {
            if (ImGui::MenuItem("Add State"))
            {
                const glm::vec2 position = ScreenToWorld(io.MousePos);
                AddState(anim);
                if (!graph.States.empty())
                {
                    graph.States.back().EditorPosition =
                        position - glm::vec2(kNodeWidth * 0.5f, kNodeHeight * 0.5f);
                    m_SelectedState = static_cast<int>(graph.States.size()) - 1;
                }
            }
            if (ImGui::MenuItem("Frame All"))
                FrameAll(anim);
            ImGui::EndPopup();
        }

        draw->PopClipRect();
    }

    void AnimationGraphPanel::DrawInspector(AnimatorComponent& anim)
    {
        AnimationGraph& graph = anim.Graph;

        if (m_SelectedState >= 0 && m_SelectedState < static_cast<int>(graph.States.size()))
            DrawStateInspector(anim);
        else if (m_SelectedTransition >= 0 && m_SelectedTransition < static_cast<int>(graph.Transitions.size()))
            DrawTransitionInspector(anim);
        else
            ImGui::TextDisabled("Select a state or a link to edit it.");

        ImGui::Separator();
        DrawParameters(graph);
    }

    void AnimationGraphPanel::DrawStateInspector(AnimatorComponent& anim)
    {
        AnimationGraph& graph = anim.Graph;
        AnimationGraphState& state = graph.States[static_cast<std::size_t>(m_SelectedState)];

        ImGui::SeparatorText("State");
        std::snprintf(m_StateNameBuffer, sizeof(m_StateNameBuffer), "%s", state.Name.c_str());
        if (ImGui::InputText("Name", m_StateNameBuffer, sizeof(m_StateNameBuffer)))
            state.Name = m_StateNameBuffer;

        ClipCombo("Clip", state.Clip, ClipNames(anim));
        ImGui::DragFloat("Speed", &state.Speed, 0.01f, 0.05f, 4.0f, "%.2fx");
        ImGui::Checkbox("Loop", &state.Loop);

        if (ImGui::Button("Set as Entry"))
            graph.EntryState = m_SelectedState;
        ImGui::SameLine();
        if (ImGui::Button("Delete State"))
        {
            RemoveState(graph, m_SelectedState);
            m_SelectedState = -1;
        }
    }

    void AnimationGraphPanel::DrawTransitionInspector(AnimatorComponent& anim)
    {
        AnimationGraph& graph = anim.Graph;
        AnimationGraphTransition& transition = graph.Transitions[static_cast<std::size_t>(m_SelectedTransition)];

        auto stateName = [&graph](int index) -> const char*
        {
            if (index >= 0 && index < static_cast<int>(graph.States.size()))
                return graph.States[static_cast<std::size_t>(index)].Name.c_str();
            return "?";
        };

        ImGui::SeparatorText("Transition");
        ImGui::Text("%s  ->  %s", stateName(transition.From), stateName(transition.To));

        if (ImGui::BeginCombo("Condition", kConditionNames[static_cast<int>(transition.Condition)]))
        {
            for (int i = 0; i < 5; ++i)
            {
                const bool selected = static_cast<int>(transition.Condition) == i;
                if (ImGui::Selectable(kConditionNames[i], selected))
                    transition.Condition = static_cast<AnimGraphCondition>(i);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const char* parameterLabel = transition.Parameter.empty() ? "(none / auto)" : transition.Parameter.c_str();
        if (ImGui::BeginCombo("Parameter", parameterLabel))
        {
            if (ImGui::Selectable("(none / auto)", transition.Parameter.empty()))
                transition.Parameter.clear();
            for (const AnimationGraphParameter& parameter : graph.Parameters)
            {
                const bool selected = transition.Parameter == parameter.Name;
                if (ImGui::Selectable(parameter.Name.c_str(), selected))
                    transition.Parameter = parameter.Name;
            }
            ImGui::EndCombo();
        }

        if (transition.Condition == AnimGraphCondition::Greater || transition.Condition == AnimGraphCondition::Less)
            ImGui::DragFloat("Threshold", &transition.Threshold, 0.01f);

        ImGui::DragFloat("Cross-fade", &transition.Duration, 0.01f, 0.0f, 2.0f, "%.2f s");
        ImGui::Checkbox("Exit Time", &transition.HasExitTime);
        if (transition.HasExitTime)
            ImGui::SliderFloat("At", &transition.ExitTime, 0.0f, 1.0f, "%.2f");

        if (ImGui::Button("Delete Transition"))
        {
            graph.Transitions.erase(graph.Transitions.begin() + m_SelectedTransition);
            m_SelectedTransition = -1;
        }
    }

    void AnimationGraphPanel::DrawParameters(AnimationGraph& graph)
    {
        ImGui::SeparatorText("Parameters");

        if (graph.Parameters.empty())
            ImGui::TextDisabled("Parameters drive transitions. Add one below.");

        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(graph.Parameters.size()); ++i)
        {
            AnimationGraphParameter& parameter = graph.Parameters[static_cast<std::size_t>(i)];
            ImGui::PushID(i);

            char name[96];
            std::snprintf(name, sizeof(name), "%s", parameter.Name.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.48f);
            if (ImGui::InputText("##name", name, sizeof(name)))
                parameter.Name = name;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 32.0f);
            ImGui::DragFloat("##value", &parameter.Value, 0.01f);
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
                removeIndex = i;

            ImGui::PopID();
        }

        if (removeIndex >= 0)
        {
            const std::string removed = graph.Parameters[static_cast<std::size_t>(removeIndex)].Name;
            graph.Parameters.erase(graph.Parameters.begin() + removeIndex);
            for (AnimationGraphTransition& transition : graph.Transitions)
            {
                if (transition.Parameter == removed)
                    transition.Parameter.clear();
            }
        }

        if (ImGui::Button("Add Parameter"))
        {
            std::snprintf(m_ParameterNameBuffer, sizeof(m_ParameterNameBuffer), "param%d",
                          static_cast<int>(graph.Parameters.size()) + 1);
            graph.Parameters.push_back({m_ParameterNameBuffer, 0.0f});
        }
    }

    void AnimationGraphPanel::AddState(AnimatorComponent& anim)
    {
        AnimationGraph& graph = anim.Graph;
        AnimationGraphState state;
        state.Name = UniqueStateName(graph);

        const std::vector<std::string> clips = ClipNames(anim);
        if (!clips.empty())
            state.Clip = clips.front();

        const glm::vec2 center = (glm::vec2(m_CanvasSize.x, m_CanvasSize.y) * 0.5f - m_Pan) / m_Zoom;
        state.EditorPosition = center - glm::vec2(kNodeWidth * 0.5f, kNodeHeight * 0.5f) +
                               glm::vec2(static_cast<float>(graph.States.size() % 4) * 24.0f,
                                         static_cast<float>(graph.States.size() / 4) * 24.0f);

        graph.States.push_back(std::move(state));
        m_SelectedState = static_cast<int>(graph.States.size()) - 1;
        m_SelectedTransition = -1;
        if (graph.EntryState < 0 || graph.EntryState >= static_cast<int>(graph.States.size()))
            graph.EntryState = 0;
    }

    void AnimationGraphPanel::RemoveState(AnimationGraph& graph, int index)
    {
        if (index < 0 || index >= static_cast<int>(graph.States.size()))
            return;

        graph.States.erase(graph.States.begin() + index);

        for (auto it = graph.Transitions.begin(); it != graph.Transitions.end();)
        {
            if (it->From == index || it->To == index)
            {
                it = graph.Transitions.erase(it);
                continue;
            }
            if (it->From > index)
                --it->From;
            if (it->To > index)
                --it->To;
            ++it;
        }

        if (graph.EntryState == index)
            graph.EntryState = 0;
        else if (graph.EntryState > index)
            --graph.EntryState;
    }

    void AnimationGraphPanel::AddTransition(AnimationGraph& graph, int from, int to)
    {
        for (const AnimationGraphTransition& existing : graph.Transitions)
        {
            if (existing.From == from && existing.To == to)
                return;
        }

        AnimationGraphTransition transition;
        transition.From = from;
        transition.To = to;

        if (!graph.Parameters.empty())
        {
            transition.Parameter = graph.Parameters.front().Name;
            transition.Condition = AnimGraphCondition::Greater;
        }
        else
        {
            // No parameters yet: wait for the clip to nearly finish instead of
            // leaving the state immediately.
            transition.HasExitTime = true;
            transition.ExitTime = 0.9f;
        }

        graph.Transitions.push_back(transition);
        m_SelectedTransition = static_cast<int>(graph.Transitions.size()) - 1;
        m_SelectedState = -1;
    }

    void AnimationGraphPanel::FrameAll(const AnimatorComponent& anim)
    {
        const AnimationGraph& graph = anim.Graph;
        if (graph.States.empty() || m_CanvasSize.x < 20.0f || m_CanvasSize.y < 20.0f)
        {
            m_Pan = glm::vec2(40.0f, 40.0f);
            m_Zoom = 1.0f;
            return;
        }

        glm::vec2 min = graph.States.front().EditorPosition;
        glm::vec2 max = min + glm::vec2(kNodeWidth, kNodeHeight);
        for (const AnimationGraphState& state : graph.States)
        {
            min = glm::min(min, state.EditorPosition);
            max = glm::max(max, state.EditorPosition + glm::vec2(kNodeWidth, kNodeHeight));
        }

        const glm::vec2 size = glm::max(max - min, glm::vec2(1.0f));
        const float zoomX = (m_CanvasSize.x - 60.0f) / size.x;
        const float zoomY = (m_CanvasSize.y - 60.0f) / size.y;
        m_Zoom = std::clamp(std::min(zoomX, zoomY), 0.4f, 1.4f);

        const glm::vec2 center = (min + max) * 0.5f;
        m_Pan = glm::vec2(m_CanvasSize.x * 0.5f, m_CanvasSize.y * 0.5f) - center * m_Zoom;
    }

    ImVec2 AnimationGraphPanel::WorldToScreen(const glm::vec2& world) const
    {
        return ImVec2(m_CanvasMin.x + m_Pan.x + world.x * m_Zoom,
                      m_CanvasMin.y + m_Pan.y + world.y * m_Zoom);
    }

    glm::vec2 AnimationGraphPanel::ScreenToWorld(const ImVec2& screen) const
    {
        return glm::vec2((screen.x - m_CanvasMin.x - m_Pan.x) / m_Zoom,
                         (screen.y - m_CanvasMin.y - m_Pan.y) / m_Zoom);
    }

    int AnimationGraphPanel::NodeAt(const AnimationGraph& graph, const glm::vec2& world) const
    {
        for (int i = static_cast<int>(graph.States.size()) - 1; i >= 0; --i)
        {
            const glm::vec2& position = graph.States[static_cast<std::size_t>(i)].EditorPosition;
            if (world.x >= position.x && world.x <= position.x + kNodeWidth && world.y >= position.y &&
                world.y <= position.y + kNodeHeight)
                return i;
        }
        return -1;
    }

    int AnimationGraphPanel::TransitionAt(const AnimationGraph& graph, const ImVec2& screen) const
    {
        for (int i = 0; i < static_cast<int>(graph.Transitions.size()); ++i)
        {
            const AnimationGraphTransition& transition = graph.Transitions[static_cast<std::size_t>(i)];
            if (transition.From < 0 || transition.From >= static_cast<int>(graph.States.size()) ||
                transition.To < 0 || transition.To >= static_cast<int>(graph.States.size()))
                continue;

            const ImVec2 fromOrigin = WorldToScreen(graph.States[static_cast<std::size_t>(transition.From)].EditorPosition);
            const ImVec2 from(fromOrigin.x + kNodeWidth * m_Zoom, fromOrigin.y + kNodeHeight * 0.5f * m_Zoom);
            const ImVec2 toOrigin = WorldToScreen(graph.States[static_cast<std::size_t>(transition.To)].EditorPosition);
            const ImVec2 to(toOrigin.x, toOrigin.y + kNodeHeight * 0.5f * m_Zoom);
            const float bend = std::max(40.0f * m_Zoom, std::abs(to.x - from.x) * 0.5f);
            const ImVec2 c1(from.x + bend, from.y);
            const ImVec2 c2(to.x - bend, to.y);

            ImVec2 previous = from;
            for (int step = 1; step <= 24; ++step)
            {
                const ImVec2 point = BezierPoint(from, c1, c2, to, static_cast<float>(step) / 24.0f);
                if (DistanceToSegment(screen, previous, point) <= 7.0f)
                    return i;
                previous = point;
            }
        }
        return -1;
    }
} // namespace UHE
