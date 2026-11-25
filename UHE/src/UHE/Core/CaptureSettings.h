#pragma once

#include <optional>
#include <string>
#include "UHE/Core/Core.h"

namespace UHE
{

/**
 * @brief Test hook that makes a run reproducible and saves one frame to disk.
 *
 * Read from the environment once at startup:
 *   - `UHE_FIXED_TIMESTEP=<seconds>`: every frame advances by exactly this much
 *     instead of wall time, so frame N shows the same scene on any machine.
 *   - `UHE_CAPTURE_FRAME=<n>`: read back the n-th rendered frame (1-based) and exit.
 *   - `UHE_CAPTURE_PATH=<file.png>`: where that frame is written (default `capture.png`).
 *
 * CI's golden-image check renders the Sandbox on lavapipe with these set and
 * compares the PNG against `tests/golden/`. Pure logic, no engine dependencies:
 * tests/capture drives the parser directly.
 */
struct UHE_API CaptureSettings
{
    std::optional<float> FixedTimestep; ///< seconds per frame; unset = wall clock
    std::optional<u32> CaptureFrame;    ///< 1-based rendered frame to capture; unset = never
    std::string CapturePath = "capture.png";

    /// @brief true when a frame is to be captured.
    [[nodiscard]] bool CaptureEnabled() const { return CaptureFrame.has_value(); }

    /**
     * @brief Parses the three variables. Each argument is the raw value or nullptr when unset.
     *
     * A value that is empty, malformed, zero or negative leaves its setting unset,
     * so a typo never turns into a capture of frame 0 or a frozen clock.
     */
    [[nodiscard]] static CaptureSettings Parse(const char* fixedTimestep, const char* captureFrame,
                                               const char* capturePath);

    /// @brief Parse() applied to the process environment.
    [[nodiscard]] static CaptureSettings FromEnvironment();
};

} // namespace UHE
