// Minimal UHE::Log statics for the glTF loader harness.
//
// The loader reports failures through the UHE::Log macros; the test needs those
// calls to link and run, not to format anywhere. Mirrors the approach in
// tests/rendergraph/log_stub.cpp - test-only, never compiled into the engine,
// where it would clash with Log.cpp.

#include <mutex>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "UHE/Core/Log.h"

namespace UHE
{

std::shared_ptr<ImGuiLogSink<std::mutex>> Log::s_ImGuiSink;
std::shared_ptr<spdlog::logger> Log::s_CoreLogger;
std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

// The UHE_* macros dereference these loggers unconditionally, so they must be
// non-null before any loader call - a null shared_ptr would crash rather than
// no-op. Warnings and above go to stderr so the test's own stdout stays clean.
void Log::Init()
{
    if (!s_CoreLogger)
    {
        s_CoreLogger = spdlog::stderr_color_mt("gltf_test_core");
        s_CoreLogger->set_level(spdlog::level::warn);
    }
    if (!s_ClientLogger)
    {
        s_ClientLogger = spdlog::stderr_color_mt("gltf_test_client");
        s_ClientLogger->set_level(spdlog::level::warn);
    }
}

} // namespace UHE
