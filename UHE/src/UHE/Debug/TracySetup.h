#pragma once
#define TRACY_ON_DEMAND
#include <tracy/Tracy.hpp>

#include "UHE/Debug/Profiler.h"

// Profiler.h defines UHE_PROFILE_SCOPE()/UHE_PROFILE_FUNCTION() as the always-on
// in-editor profiler scopes. In profile builds each scope additionally emits a
// Tracy zone; the macros expand to declarations, so they must be used as a
// statement ("UHE_PROFILE_SCOPE("x");").
#if defined(UHE_PROFILE) && UHE_PROFILE
#undef UHE_PROFILE_SCOPE
#undef UHE_PROFILE_FUNCTION

#define UHE_PROFILE_SCOPE(name)                                                                \
    ::UHE::ProfilerScope UHE_PROFILE_CONCAT(uheProfilerScope_, __LINE__)(name, __FILE__);      \
    ZoneScopedN(name)
#define UHE_PROFILE_FUNCTION()                                                                 \
    ::UHE::ProfilerScope UHE_PROFILE_CONCAT(uheProfilerScope_, __LINE__)(__FUNCTION__,         \
                                                                         __FILE__);            \
    ZoneScoped

#define UHE_PROFILE_FRAMEMARK FrameMark
#define UHE_GPU_ZONE(name) ZoneScopedN(name)
#else
#define UHE_PROFILE_FRAMEMARK
#define UHE_GPU_ZONE(name)
#endif
