#pragma once

#include <cstdint>

namespace UHE
{

/// Fixed-step simulation clock (issue #49).
///
/// Rendering runs at whatever rate the GPU and the display allow; the
/// simulation (scripts and physics) must not. Frame time is fed in with
/// Advance(), which returns how many fixed ticks to run now. Time is kept in
/// integer nanoseconds, so the same sequence of frame times always yields the
/// same sequence of ticks, and the tick index is the simulation's clock:
/// after N ticks the state is the same whether they came from 30 FPS or 240.
///
/// Pure logic, no engine dependencies: tests/scene drives it directly.
class SimulationClock
{
public:
    static constexpr std::uint32_t kDefaultTickRate = 120;
    /// The application already clamps a frame to 0.25 s; at the default rate
    /// that is 30 ticks, so one hitch is caught up in full and anything longer
    /// is dropped instead of snowballing into ever-longer frames.
    static constexpr std::uint32_t kDefaultMaxTicksPerAdvance = 30;

    explicit SimulationClock(std::uint32_t ticksPerSecond = kDefaultTickRate,
                             std::uint32_t maxTicksPerAdvance = kDefaultMaxTicksPerAdvance);

    /// Sets the fixed step from a rate. Clears the accumulated remainder.
    void SetTickRate(std::uint32_t ticksPerSecond);
    /// Sets the fixed step directly (seconds, rounded to whole nanoseconds,
    /// at least one). Clears the accumulated remainder.
    void SetFixedStep(double seconds);
    void SetMaxTicksPerAdvance(std::uint32_t maxTicks);

    /// Adds one rendered frame's duration and returns how many fixed ticks to
    /// run for it, at most GetMaxTicksPerAdvance(). Time beyond that cap is
    /// discarded (the simulation runs slow rather than falling further behind).
    /// A paused clock accumulates nothing and returns 0. The caller runs the
    /// ticks and reports each with OnTickCompleted().
    [[nodiscard]] std::uint32_t Advance(double frameSeconds);

    /// Counts one completed tick: from Advance(), or a manual single step.
    void OnTickCompleted() { ++m_TickIndex; }

    /// How far the accumulator is into the next tick, in [0, 1). Rendering
    /// blends the last two simulated states by this much.
    [[nodiscard]] float Alpha() const;

    void SetPaused(bool paused) { m_Paused = paused; }
    [[nodiscard]] bool IsPaused() const { return m_Paused; }

    /// Back to tick 0 with an empty accumulator; the rate is kept.
    void Reset();

    [[nodiscard]] double GetFixedStep() const { return static_cast<double>(m_StepNs) * 1e-9; }
    [[nodiscard]] std::int64_t GetFixedStepNanoseconds() const { return m_StepNs; }
    [[nodiscard]] std::uint32_t GetMaxTicksPerAdvance() const { return m_MaxTicks; }
    /// Ticks completed since the clock started (or was Reset).
    [[nodiscard]] std::uint64_t GetTickIndex() const { return m_TickIndex; }
    /// Simulated time: tick index times the fixed step.
    [[nodiscard]] double GetSimulatedSeconds() const
    {
        return static_cast<double>(m_TickIndex) * GetFixedStep();
    }

private:
    std::int64_t m_StepNs = 0;
    std::int64_t m_AccumulatorNs = 0;
    std::uint32_t m_MaxTicks = kDefaultMaxTicksPerAdvance;
    std::uint64_t m_TickIndex = 0;
    bool m_Paused = false;
};

} // namespace UHE
