#include "AnimationStateMachine.h"
#include <algorithm>

namespace UHE::RD3d {

void AnimationStateMachine::AddParameter(const std::string& name, ParameterValue initial)
{
    m_Parameters[name] = initial;
}

void AnimationStateMachine::SetBool(const std::string& name, bool value)
{
    m_Parameters[name] = value;
}

void AnimationStateMachine::SetInt(const std::string& name, int value)
{
    m_Parameters[name] = value;
}

void AnimationStateMachine::SetFloat(const std::string& name, float value)
{
    m_Parameters[name] = value;
}

bool AnimationStateMachine::GetBool(const std::string& name) const
{
    const ParameterValue* v = FindParameter(name);
    return v && std::holds_alternative<bool>(*v) ? std::get<bool>(*v) : false;
}

int AnimationStateMachine::GetInt(const std::string& name) const
{
    const ParameterValue* v = FindParameter(name);
    return v && std::holds_alternative<int>(*v) ? std::get<int>(*v) : 0;
}

float AnimationStateMachine::GetFloat(const std::string& name) const
{
    const ParameterValue* v = FindParameter(name);
    return v && std::holds_alternative<float>(*v) ? std::get<float>(*v) : 0.0f;
}

void AnimationStateMachine::SetTrigger(const std::string& name)
{
    m_Triggers[name] = true;
}

void AnimationStateMachine::ResetTrigger(const std::string& name)
{
    m_Triggers[name] = false;
}

void AnimationStateMachine::AddState(const State& state)
{
    for (auto& existing : m_States)
    {
        if (existing.Name == state.Name)
        {
            existing = state;
            return;
        }
    }
    m_States.push_back(state);
}

void AnimationStateMachine::AddTransition(const Transition& transition)
{
    m_Transitions.push_back(transition);
}

bool AnimationStateMachine::HasState(const std::string& name) const
{
    return std::any_of(m_States.begin(), m_States.end(),
                       [&](const State& s) { return s.Name == name; });
}

void AnimationStateMachine::Enter(const std::string& stateName, Animator& animator, float blendDuration)
{
    const State* target = nullptr;
    for (const auto& state : m_States)
    {
        if (state.Name == stateName)
        {
            target = &state;
            break;
        }
    }
    if (!target)
        return;

    m_CurrentState = stateName;
    ApplyState(*target, animator, blendDuration);
}

void AnimationStateMachine::Update(Animator& animator)
{
    if (m_CurrentState.empty())
    {
        // No state entered yet: fall into the first registered one.
        if (m_States.empty())
            return;
        m_CurrentState = m_States.front().Name;
        ApplyState(m_States.front(), animator, 0.0f);
        return;
    }

    const State* current = nullptr;
    for (const auto& state : m_States)
    {
        if (state.Name == m_CurrentState)
        {
            current = &state;
            break;
        }
    }
    if (!current)
        return;

    // Respect the state's clip speed while it plays.
    animator.SetTimeScale(current->Speed);

    for (const auto& transition : m_Transitions)
    {
        if (transition.From != m_CurrentState)
            continue;

        if (transition.HasExitTime && animator.GetNormalizedTime() < transition.ExitTime)
            continue;

        bool pass = true;
        for (const auto& condition : transition.Conditions)
        {
            if (!EvaluateCondition(condition))
            {
                pass = false;
                break;
            }
        }
        if (!pass)
            continue;

        for (const auto& state : m_States)
        {
            if (state.Name == transition.To)
            {
                m_CurrentState = transition.To;
                ApplyState(state, animator, transition.BlendDuration);
                // Consume every trigger this transition relied on so a single
                // SetTrigger fires it exactly once.
                for (const auto& condition : transition.Conditions)
                {
                    if (condition.Operator == Condition::Op::Equals &&
                        std::holds_alternative<bool>(condition.Value) &&
                        std::get<bool>(condition.Value))
                    {
                        auto it = m_Triggers.find(condition.Parameter);
                        if (it != m_Triggers.end())
                            it->second = false;
                    }
                }
                break; // one transition per Update
            }
        }
        return;
    }
}

bool AnimationStateMachine::EvaluateCondition(const Condition& condition) const
{
    // Trigger conditions read from the trigger table; everything else from
    // the parameter table.
    if (condition.Operator == Condition::Op::Equals && std::holds_alternative<bool>(condition.Value) &&
        std::get<bool>(condition.Value) && m_Triggers.count(condition.Parameter))
        return m_Triggers.at(condition.Parameter);

    const ParameterValue* value = FindParameter(condition.Parameter);
    if (!value)
        return false;

    auto compare = [&](auto a, auto b)
    {
        switch (condition.Operator)
        {
        case Condition::Op::Equals: return a == b;
        case Condition::Op::NotEquals: return a != b;
        case Condition::Op::Greater: return a > b;
        case Condition::Op::Less: return a < b;
        }
        return false;
    };

    if (auto* expected = std::get_if<bool>(&condition.Value))
        return std::holds_alternative<bool>(*value) && compare(std::get<bool>(*value), *expected);
    if (auto* expected = std::get_if<int>(&condition.Value))
        return std::holds_alternative<int>(*value) && compare(std::get<int>(*value), *expected);
    if (auto* expected = std::get_if<float>(&condition.Value))
        return std::holds_alternative<float>(*value) && compare(std::get<float>(*value), *expected);
    return false;
}

void AnimationStateMachine::ApplyState(const State& state, Animator& animator, float blendDuration)
{
    animator.SetLoopMode(state.Loop);
    animator.SetTimeScale(state.Speed);
    if (blendDuration <= 0.0f)
        animator.PlayAnimation(state.ClipName);
    else
        animator.CrossFade(state.ClipName, blendDuration);
}

const AnimationStateMachine::ParameterValue* AnimationStateMachine::FindParameter(const std::string& name) const
{
    auto it = m_Parameters.find(name);
    return it != m_Parameters.end() ? &it->second : nullptr;
}

} // namespace UHE::RD3d
