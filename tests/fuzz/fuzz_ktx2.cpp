/**
 * @file fuzz_ktx2.cpp
 * @brief libFuzzer target: KTX2 / Basis Universal decoding (issue #42).
 *
 * A texture in a project or a cook is untrusted input to LoadKTX2; it must
 * refuse a damaged file rather than crash the editor or the game.
 */
#include <cstddef>
#include <cstdint>
#include "UHE/Core/Log.h"
#include "UHE/Renderer/KTX2.h"

/// @brief Sets up the loggers the parsers report refusals through, silenced.
extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    UHE::Log::Init();
    UHE::Log::GetCoreLogger()->set_level(spdlog::level::off); // a refused input is the expected path
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (!UHE::IsKTX2(data, size))
        return 0;
    UHE::KTX2Image image;
    (void)UHE::LoadKTX2(data, size, image);
    return 0;
}
