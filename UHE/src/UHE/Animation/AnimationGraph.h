#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE::RD3d
{
class Animator;
class AnimationStateMachine;
} // namespace UHE::RD3d

namespace UHE
{
// ─── Animation graph (state machine) ────────────────────────────────────
// A node per state, a link per transition, and named parameters that drive
// the transitions. This mirrors the useful half of Godot's AnimationTree:
// clips are still clips, but which one plays is decided by the graph instead
// of a hard-coded name on the component.

enum class AnimGraphCondition : int
{
    Greater = 0, // parameter > threshold
    Less,        // parameter < threshold
    True,        // parameter > 0.5
    False,       // parameter <= 0.5
    Trigger,     // parameter > 0.5, consumed when taken
};

struct AnimationGraphState
{
    std::string Name = "State";
    std::string Clip; // clip name from the entity's model
    glm::vec2 EditorPosition{0.0f, 0.0f};
    f32 Speed = 1.0f;
    bool Loop = true;
};

struct AnimationGraphTransition
{
    int From = -1;
    int To = -1;
    std::string Parameter; // empty = unconditional (auto-advance)
    AnimGraphCondition Condition = AnimGraphCondition::Greater;
    f32 Threshold = 0.5f;
    f32 Duration = 0.2f; // cross-fade seconds
    bool HasExitTime = false;
    f32 ExitTime = 1.0f; // normalized clip time
};

struct AnimationGraphParameter
{
    std::string Name;
    f32 Value = 0.0f;
};

struct AnimationGraph
{
    bool Enabled = false;
    int EntryState = 0;
    std::vector<AnimationGraphState> States;
    std::vector<AnimationGraphTransition> Transitions;
    std::vector<AnimationGraphParameter> Parameters;

    [[nodiscard]] int FindParameter(const std::string& name) const;
    [[nodiscard]] f32 GetParameter(const std::string& name) const;
    void SetParameter(const std::string& name, f32 value);
    [[nodiscard]] bool ConsumeTrigger(const std::string& name);
};

// Compiles the editor-authored graph into the engine's tested
// AnimationStateMachine. Rebuild only when HashGraph() changes.
UHE_API std::shared_ptr<RD3d::AnimationStateMachine> BuildStateMachine(const AnimationGraph& graph);

// Pushes parameter values into the compiled machine. Trigger parameters
// (value > 0.5) are consumed and become one-shot triggers.
UHE_API void SyncStateMachineParameters(AnimationGraph& graph, RD3d::AnimationStateMachine& machine);

// Cheap FNV hash of everything that affects the compiled machine, so an
// editor edit triggers a rebuild and an unchanged graph does not.
UHE_API u64 HashGraph(const AnimationGraph& graph);
} // namespace UHE
