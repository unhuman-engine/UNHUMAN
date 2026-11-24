#include "uhepch.h"
#include "AnimationGraph.h"

#include "UHE/Renderer3D/AnimationStateMachine.h"

#include <algorithm>
#include <cstring>

namespace UHE
{
    int AnimationGraph::FindParameter(const std::string& name) const
    {
        for (int i = 0; i < static_cast<int>(Parameters.size()); ++i)
        {
            if (Parameters[static_cast<std::size_t>(i)].Name == name)
                return i;
        }
        return -1;
    }

    f32 AnimationGraph::GetParameter(const std::string& name) const
    {
        const int index = FindParameter(name);
        return index >= 0 ? Parameters[static_cast<std::size_t>(index)].Value : 0.0f;
    }

    void AnimationGraph::SetParameter(const std::string& name, f32 value)
    {
        const int index = FindParameter(name);
        if (index >= 0)
            Parameters[static_cast<std::size_t>(index)].Value = value;
    }

    bool AnimationGraph::ConsumeTrigger(const std::string& name)
    {
        const int index = FindParameter(name);
        if (index < 0 || Parameters[static_cast<std::size_t>(index)].Value <= 0.5f)
            return false;

        Parameters[static_cast<std::size_t>(index)].Value = 0.0f;
        return true;
    }

    namespace
    {
        enum class ParameterKind
        {
            Float,
            Boolean,
        };

        bool UsesParameter(const AnimationGraphTransition& transition, const std::string& name)
        {
            return transition.Parameter == name;
        }

        bool HasTriggerCondition(const AnimationGraph& graph, const std::string& name)
        {
            return std::any_of(graph.Transitions.begin(), graph.Transitions.end(),
                               [&](const AnimationGraphTransition& transition)
                               {
                                   return UsesParameter(transition, name) &&
                                          transition.Condition == AnimGraphCondition::Trigger;
                               });
        }

        ParameterKind KindOf(const AnimationGraph& graph, const std::string& name)
        {
            for (const AnimationGraphTransition& transition : graph.Transitions)
            {
                if (!UsesParameter(transition, name))
                    continue;
                if (transition.Condition == AnimGraphCondition::Greater ||
                    transition.Condition == AnimGraphCondition::Less)
                    return ParameterKind::Float;
            }
            return ParameterKind::Boolean;
        }

        const char* StateName(const AnimationGraph& graph, int index)
        {
            if (index < 0 || index >= static_cast<int>(graph.States.size()))
                return nullptr;
            return graph.States[static_cast<std::size_t>(index)].Name.c_str();
        }
    } // namespace

    std::shared_ptr<RD3d::AnimationStateMachine> BuildStateMachine(const AnimationGraph& graph)
    {
        auto machine = std::make_shared<RD3d::AnimationStateMachine>();

        for (const AnimationGraphState& state : graph.States)
        {
            machine->AddState({state.Name, state.Clip, state.Speed,
                               state.Loop ? RD3d::LoopMode::Loop : RD3d::LoopMode::None});
        }

        for (const AnimationGraphParameter& parameter : graph.Parameters)
        {
            if (KindOf(graph, parameter.Name) == ParameterKind::Float)
                machine->AddParameter(parameter.Name, parameter.Value);
            else
                machine->AddParameter(parameter.Name, parameter.Value > 0.5f);
        }

        for (const AnimationGraphTransition& transition : graph.Transitions)
        {
            const char* from = StateName(graph, transition.From);
            const char* to = StateName(graph, transition.To);
            if (!from || !to)
                continue;

            std::vector<RD3d::AnimationStateMachine::Condition> conditions;
            if (!transition.Parameter.empty())
            {
                using Condition = RD3d::AnimationStateMachine::Condition;
                switch (transition.Condition)
                {
                    case AnimGraphCondition::Greater:
                        conditions.push_back(
                            {transition.Parameter, Condition::Op::Greater, transition.Threshold});
                        break;
                    case AnimGraphCondition::Less:
                        conditions.push_back({transition.Parameter, Condition::Op::Less, transition.Threshold});
                        break;
                    case AnimGraphCondition::True:
                    case AnimGraphCondition::Trigger:
                        conditions.push_back({transition.Parameter, Condition::Op::Equals, true});
                        break;
                    case AnimGraphCondition::False:
                        conditions.push_back({transition.Parameter, Condition::Op::Equals, false});
                        break;
                }
            }

            machine->AddTransition({from, to, std::move(conditions), transition.Duration, transition.HasExitTime,
                                    transition.ExitTime});
        }

        return machine;
    }

    void SyncStateMachineParameters(AnimationGraph& graph, RD3d::AnimationStateMachine& machine)
    {
        for (AnimationGraphParameter& parameter : graph.Parameters)
        {
            if (KindOf(graph, parameter.Name) == ParameterKind::Float)
            {
                machine.SetFloat(parameter.Name, parameter.Value);
                continue;
            }

            if (HasTriggerCondition(graph, parameter.Name) && parameter.Value > 0.5f)
            {
                machine.SetTrigger(parameter.Name);
                parameter.Value = 0.0f; // one-shot
            }
            else
            {
                machine.SetBool(parameter.Name, parameter.Value > 0.5f);
            }
        }
    }

    u64 HashGraph(const AnimationGraph& graph)
    {
        u64 hash = 1469598103934665603ull; // FNV-1a

        auto bytes = [&hash](const void* data, std::size_t size)
        {
            const auto* values = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= values[i];
                hash *= 1099511628211ull;
            }
        };

        auto text = [&bytes](const std::string& value)
        {
            bytes(value.data(), value.size());
            const unsigned char separator = 0xFF;
            bytes(&separator, 1);
        };

        auto number = [&bytes](f32 value) { bytes(&value, sizeof(value)); };
        auto integer = [&bytes](int value) { bytes(&value, sizeof(value)); };
        auto flag = [&bytes](bool value)
        {
            const unsigned char byte = value ? 1 : 0;
            bytes(&byte, 1);
        };

        integer(static_cast<int>(graph.States.size()));
        for (const AnimationGraphState& state : graph.States)
        {
            text(state.Name);
            text(state.Clip);
            number(state.Speed);
            flag(state.Loop);
        }

        integer(static_cast<int>(graph.Transitions.size()));
        for (const AnimationGraphTransition& transition : graph.Transitions)
        {
            integer(transition.From);
            integer(transition.To);
            text(transition.Parameter);
            integer(static_cast<int>(transition.Condition));
            number(transition.Threshold);
            number(transition.Duration);
            flag(transition.HasExitTime);
            number(transition.ExitTime);
        }

        integer(static_cast<int>(graph.Parameters.size()));
        for (const AnimationGraphParameter& parameter : graph.Parameters)
            text(parameter.Name);

        integer(graph.EntryState);
        return hash;
    }
} // namespace UHE
