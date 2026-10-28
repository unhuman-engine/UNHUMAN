#pragma once

#include "UHE/Core/Core.h"

#include <string>
#include <vector>

namespace UHE
{
    // ─── Profiler ───────────────────────────────────────────────────────────
    // Frame profiler behind UHE_PROFILE_SCOPE()/UHE_PROFILE_FUNCTION().
    //
    // Every scope records one (name, module, start, end, depth, thread) sample
    // into a fixed-size per-thread buffer (no allocation, no lock on the hot
    // path). Profiler::BeginFrame()/EndFrame() bracket a frame; EndFrame()
    // drains the per-thread buffers and rebuilds the aggregates the editor's
    // Profiler panel reads: per-scope totals/self time/percentages, per-thread
    // busy time, the call tree of the captured frame and the frame-time history.
    //
    // Cost when disabled: one relaxed atomic load per scope.
    // Cost when enabled: two clock reads plus one slot per scope.
    //
    // Threading: scopes may be recorded from any thread. Aggregation and the
    // read API are main-thread only (the editor UI thread).

    using ProfileThreadId = u32;

    struct ProfileSample
    {
        const char* Name = nullptr;   // literal from the call site
        const char* Module = nullptr; // __FILE__ of the call site
        // Absolute milliseconds since process start: f64 so a long session does
        // not quantize short scopes (f32 loses sub-ms resolution past ~4e6 ms).
        f64 StartMs = 0.0;
        f64 EndMs = 0.0;
        u16 Depth = 0;
        u16 Reserved = 0;
        ProfileThreadId Thread = 0;
    };

    // One node of the captured frame's call tree, in start (pre-)order.
    struct ProfileNode
    {
        std::string Name;
        std::string Module;
        f64 StartMs = 0.0;  // absolute, for the timeline tooltip
        f32 TotalMs = 0.0f; // durations stay f32: they are always small
        f32 SelfMs = 0.0f;
        u16 Depth = 0;
        i32 Parent = -1;
        i32 FirstChild = -1;
        i32 NextSibling = -1;
        ProfileThreadId Thread = 0;
    };

    // Aggregated over the averaging window, keyed by (name, module).
    struct ProfileScopeStats
    {
        std::string Name;
        std::string Module;       // basename of __FILE__, e.g. "Renderer3D.cpp"
        f32 TotalMs = 0.0f;       // summed over the captured frame
        f32 SelfMs = 0.0f;        // TotalMs minus direct children
        f32 AverageMs = 0.0f;     // average per-frame total over the window
        f32 MinMs = 0.0f;         // since ResetHistory()
        f32 MaxMs = 0.0f;         // since ResetHistory()
        f32 FramePercent = 0.0f;  // TotalMs / captured frame time
        u32 Calls = 0;            // call count in the captured frame
        u32 Threads = 0;          // distinct threads seen in the captured frame
        f32 MainThreadMs = 0.0f;  // part of TotalMs recorded on the main thread
    };

    struct ProfileThreadStats
    {
        ProfileThreadId Id = 0;
        std::string Name;
        f32 BusyMs = 0.0f; // summed scope time in the captured frame
        u32 Samples = 0;
    };

    struct ProfileFrameRecord
    {
        f32 FrameMs = 0.0f;
        f32 Fps = 0.0f;
        f32 ScopedCpuMs = 0.0f; // top-level main-thread scope time
        u32 SampleCount = 0;
        u32 DroppedSamples = 0;
    };

    class UHE_API Profiler
    {
    public:
        // Frame boundaries; called by the application loop.
        static void BeginFrame();
        static void EndFrame();

        // Master switch: when disabled scopes cost a single atomic load.
        static void SetEnabled(bool enabled);
        [[nodiscard]] static bool IsEnabled();

        // Capture pause. While paused the last captured frame stays readable.
        static void SetCapturing(bool capturing);
        [[nodiscard]] static bool IsCapturing();

        // Drops all aggregates/history and restarts the averaging window.
        static void ResetHistory();

        // Labels the calling thread for the panel's thread breakdown.
        static void SetThreadName(const char* name);
        [[nodiscard]] static ProfileThreadId GetMainThreadId();

        // ─── Recording API (used by ProfilerScope) ──────────────────────────
        // Returns the scope's start time in ms, or a negative value when the
        // scope must not be recorded (disabled/paused).
        static f64 BeginScope();
        static void EndScope(const char* name, const char* module, f64 startMs);
        [[nodiscard]] static f64 NowMs();

        // ─── Read-only view for the UI (main thread) ────────────────────────
        [[nodiscard]] static const std::vector<ProfileScopeStats>& GetScopeStats();
        [[nodiscard]] static const std::vector<ProfileNode>& GetFrameTree();
        [[nodiscard]] static const std::vector<ProfileThreadStats>& GetThreadStats();
        [[nodiscard]] static const std::vector<ProfileFrameRecord>& GetFrameHistory();

        [[nodiscard]] static f32 GetLastFrameMs();
        [[nodiscard]] static f32 GetAverageFrameMs();
        [[nodiscard]] static f32 GetMinFrameMs();
        [[nodiscard]] static f32 GetMaxFrameMs();
        [[nodiscard]] static f32 GetScopedCpuMs();
        [[nodiscard]] static u32 GetFrameSampleCount();
        [[nodiscard]] static u32 GetDroppedSampleCount();
        [[nodiscard]] static u64 GetCapturedFrameCount();
        [[nodiscard]] static u32 GetAveragingWindow();
        [[nodiscard]] static const char* GetThreadName(ProfileThreadId id);
    };

    // RAII scope. Feeds the profiler, and Tracy when UHE_PROFILE is enabled
    // (see TracySetup.h, which wraps these macros).
    class UHE_API ProfilerScope
    {
    public:
        ProfilerScope(const char* name, const char* module);
        ~ProfilerScope();

        ProfilerScope(const ProfilerScope&) = delete;
        ProfilerScope& operator=(const ProfilerScope&) = delete;

    private:
        const char* m_Name;
        const char* m_Module;
        f64 m_StartMs;
    };
} // namespace UHE

#define UHE_PROFILE_CONCAT_IMPL(a, b) a##b
#define UHE_PROFILE_CONCAT(a, b) UHE_PROFILE_CONCAT_IMPL(a, b)

// Always-on instrumentation; TracySetup.h adds the Tracy zones on top when
// UHE_PROFILE is enabled. The macro expands to a declaration, so it must be
// used as a statement ("UHE_PROFILE_SCOPE("x");").
#define UHE_PROFILE_SCOPE(name)                                                                \
    ::UHE::ProfilerScope UHE_PROFILE_CONCAT(uheProfilerScope_, __LINE__)(name, __FILE__)
#define UHE_PROFILE_FUNCTION()                                                                 \
    ::UHE::ProfilerScope UHE_PROFILE_CONCAT(uheProfilerScope_, __LINE__)(__FUNCTION__, __FILE__)
