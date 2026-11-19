#pragma once

#include "UHE.h"
#include "imgui.h"
#include "UHE/Scene/Components.h"
#include "UHE/Scene/Entity.h"

#include <glm/glm.hpp>

namespace UHE
{
    // Node editor for AnimatorComponent::Graph: states are nodes, transitions are
    // links, and named parameters decide when a link is taken. Everything shown
    // here is the data the runtime graph evaluates, so the panel is not a mock.
    class AnimationGraphPanel
    {
    public:
        void OnImGuiRender(bool& open, Entity selected);

    private:
        void DrawToolbar(AnimatorComponent& anim);
        void DrawCanvas(AnimatorComponent& anim);
        void DrawInspector(AnimatorComponent& anim);
        void DrawParameters(AnimationGraph& graph);
        void DrawStateInspector(AnimatorComponent& anim);
        void DrawTransitionInspector(AnimatorComponent& anim);

        void AddState(AnimatorComponent& anim);
        void RemoveState(AnimationGraph& graph, int index);
        void AddTransition(AnimationGraph& graph, int from, int to);
        void FrameAll(const AnimatorComponent& anim);

        [[nodiscard]] ImVec2 WorldToScreen(const glm::vec2& world) const;
        [[nodiscard]] glm::vec2 ScreenToWorld(const ImVec2& screen) const;
        [[nodiscard]] int NodeAt(const AnimationGraph& graph, const glm::vec2& world) const;
        [[nodiscard]] int TransitionAt(const AnimationGraph& graph, const ImVec2& screen) const;

        ImVec2 m_CanvasMin{0.0f, 0.0f};
        ImVec2 m_CanvasSize{0.0f, 0.0f};
        glm::vec2 m_Pan{40.0f, 40.0f};
        float m_Zoom = 1.0f;

        int m_SelectedState = -1;
        int m_SelectedTransition = -1;
        int m_LinkFrom = -1;
        int m_DragState = -1;
        glm::vec2 m_DragOffset{0.0f};

        u64 m_ContextEntity = 0;
        bool m_FramePending = true;
        char m_StateNameBuffer[96] = {};
        char m_ParameterNameBuffer[96] = {};
    };
} // namespace UHE
