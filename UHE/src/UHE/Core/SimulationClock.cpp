#include "SimulationClock.h"
#include <algorithm>
#include <cmath>

namespace UHE
{

namespace
{
constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
}

SimulationClock::SimulationClock(std::uint32_t ticksPerSecond, std::uint32_t maxTicksPerAdvance)
{
    SetTickRate(ticksPerSecond);
    SetMaxTicksPerAdvance(maxTicksPerAdvance);
}

void SimulationClock::SetTickRate(std::uint32_t ticksPerSecond)
{
    const std::int64_t rate = std::max<std::int64_t>(1, ticksPerSecond);
    // Rounded to the nearest nanosecond: 120 Hz is 8'333'333 ns, a drift of
    // 40 ns per simulated second, far below anything a robot or a game sees.
    m_StepNs = std::max<std::int64_t>(1, (kNanosecondsPerSecond + rate / 2) / rate);
    m_AccumulatorNs = 0;
}

void SimulationClock::SetFixedStep(double seconds)
{
    const double ns = std::round(std::max(0.0, seconds) * static_cast<double>(kNanosecondsPerSecond));
    m_StepNs = std::max<std::int64_t>(1, static_cast<std::int64_t>(ns));
    m_AccumulatorNs = 0;
}

void SimulationClock::SetMaxTicksPerAdvance(std::uint32_t maxTicks)
{
    m_MaxTicks = std::max<std::uint32_t>(1, maxTicks);
}

std::uint32_t SimulationClock::Advance(double frameSeconds)
{
    if (m_Paused || !(frameSeconds > 0.0)) // also rejects NaN
    {
        return 0;
    }

    // Each frame is converted to integer nanoseconds once, so the accumulator
    // never carries floating-point error from one frame to the next.
    m_AccumulatorNs +=
        static_cast<std::int64_t>(std::llround(frameSeconds * static_cast<double>(kNanosecondsPerSecond)));

    const std::int64_t due = m_AccumulatorNs / m_StepNs;
    const std::int64_t ticks = std::min<std::int64_t>(due, m_MaxTicks);
    m_AccumulatorNs -= ticks * m_StepNs;
    if (due > ticks)
    {
        // Over the cap: drop the backlog but keep the phase within one tick.
        m_AccumulatorNs %= m_StepNs;
    }
    return static_cast<std::uint32_t>(ticks);
}

float SimulationClock::Alpha() const
{
    return static_cast<float>(static_cast<double>(m_AccumulatorNs) / static_cast<double>(m_StepNs));
}

void SimulationClock::Reset()
{
    m_AccumulatorNs = 0;
    m_TickIndex = 0;
}

} // namespace UHE
