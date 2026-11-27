/**
 * @file fuzz_cooked_container.cpp
 * @brief libFuzzer target: the cooked-file container (header, section table, zstd, CRC).
 *
 * Feeds arbitrary bytes to Cooked::Reader::OpenMemory and reads every section
 * it lists. A damaged file must be refused, never read out of bounds or
 * allocate without limit (issue #45).
 */
#include <cstddef>
#include <cstdint>
#include <vector>
#include "UHE/AssetsManager/CookedFile.h"
#include "UHE/Core/Log.h"

/// @brief Sets up the loggers the parsers report refusals through, silenced.
extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    UHE::Log::Init();
    UHE::Log::GetCoreLogger()->set_level(spdlog::level::off); // a refused input is the expected path
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    UHE::Cooked::Reader reader;
    std::string error;
    if (!reader.OpenMemory(std::vector<u8>(data, data + size), &error))
        return 0;
    std::vector<u8> section;
    for (const UHE::Cooked::SectionInfo& info : reader.Sections())
        (void)reader.ReadSection(info.Id, section, &error);
    return 0;
}
