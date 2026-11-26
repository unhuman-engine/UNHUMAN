#pragma once

#include <string>
#include "UHE/Core/Core.h"

namespace UHE::RenderDoc
{

/**
 * @brief RenderDoc's in-application API: capture frames from inside the engine.
 *
 * Init() runs before the Vulkan instance exists (RenderDoc hooks instance
 * creation). It attaches to RenderDoc when RenderDoc launched the process,
 * and loads it itself when asked with `-renderdoc` or `UHE_RENDERDOC=1`;
 * otherwise nothing is loaded and every call here is a no-op. Captures go to
 * `-renderdoc-out <path prefix>` (default `captures/uhe`), and
 * `-renderdoc-frame <n>` captures frame n without any input, for scripted
 * runs. Open a capture with qrenderdoc, or LaunchReplayUI().
 */
UHE_API void Init();

/// @brief RenderDoc is loaded and hooked into this process.
[[nodiscard]] UHE_API bool Available();

/**
 * @brief Captures the next presented frame. F12 does the same from the keyboard.
 * @param note Saved in the capture's comments (what was being looked at), shown when RenderDoc opens it.
 */
UHE_API void TriggerCapture(const std::string& note = {});

/// @brief Call once per frame: triggers the `-renderdoc-frame` capture and logs captures as they are written.
UHE_API void OnFrame(u64 frameIndex);

/// @brief How many captures this process has written, and the path of the last one ("" before any).
[[nodiscard]] UHE_API u32 CaptureCount();
[[nodiscard]] UHE_API std::string LastCapturePath();

/// @brief Opens the RenderDoc UI connected to this process; false when it could not.
UHE_API bool LaunchReplayUI();

} // namespace UHE::RenderDoc
