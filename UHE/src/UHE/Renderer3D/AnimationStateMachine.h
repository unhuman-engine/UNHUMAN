#pragma once
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include "UHE/Core/Core.h"
#include "UHE/Renderer3D/Animator.h"

namespace UHE::RD3d
{

// Issue #41: a small finite state machine driving an Animator through
// cross-faded clip states. Transitions are gated by parameters (bool/int/
// float) and triggers; a transition fires when all of its conditions hold
// and, optionally, once the current state's exit time has been reached.
class UHE_API AnimationStateMachine
{
public:
    using ParameterValue = std::variant<bool, int, float>;

    struct UHE_API Condition
    {
        enum class Op : unsigned char { Equals, NotEquals, Greater, Less };

        std::string Parameter;
        Op Operator = Op::Equals;
        ParameterValue Value = false;
    };

    struct UHE_API Transition
    {
        std::string From;
        std::string To;
        std::vector<Condition> Conditions; // AND-combined
        float BlendDuration = 0.25f;
        bool HasExitTime = false;
        float ExitTime = 1.0f; // normalized time the transition may fire at
    };

    struct UHE_API State
    {
        std::string Name;
        std::string ClipName; // clip the Animator plays in this state
        float Speed = 1.0f;   // Animator time scale while in this state
        LoopMode Loop = LoopMode::Loop;
    };

    // ---- parameters ----
    void AddParameter(const std::string& name, ParameterValue initial = false);
    void SetBool(const std::string& name, bool value);
    void SetInt(const std::string& name, int value);
    void SetFloat(const std::string& name, float value);
    bool GetBool(const std::string& name) const;
    int GetInt(const std::string& name) const;
    float GetFloat(const std::string& name) const;

    // Triggers are one-shot booleans: consumed by the transition that used
    // them, so a single SetTrigger cannot re-fire a transition forever.
    void SetTrigger(const std::string& name);
    void ResetTrigger(const std::string& name);

    // ---- states & transitions ----
    void AddState(const State& state);
    void AddTransition(const Transition& transition);
    const std::string& GetCurrentState() const { return m_CurrentState; }
    bool HasState(const std::string& name) const;

    // Jumps to a state immediately (no exit-time/condition checks) with a
    // cross-fade of 'blendDuration' seconds.
    void Enter(const std::string& stateName, Animator& animator, float blendDuration = 0.25f);

    // Evaluates transitions from the current state and applies the first one
    // whose conditions hold. Call once per frame after the Animator update.
    void Update(Animator& animator);

private:
    const ParameterValue* FindParameter(const std::string& name) const;
    bool EvaluateCondition(const Condition& condition) const;
    void ApplyState(const State& state, Animator& animator, float blendDuration);

    std::unordered_map<std::string, ParameterValue> m_Parameters;
    std::unordered_map<std::string, bool> m_Triggers;
    std::vector<State> m_States;
    std::vector<Transition> m_Transitions;
    std::string m_CurrentState;
};

} // namespace UHE::RD3d
