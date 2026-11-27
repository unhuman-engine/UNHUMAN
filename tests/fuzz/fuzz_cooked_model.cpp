/**
 * @file fuzz_cooked_model.cpp
 * @brief libFuzzer target: the model sections of a cooked model (issue #45).
 *
 * A byte flip in a real file fails the section CRC and never reaches the model
 * reader, so mutating whole files tests only the container. This target starts
 * from a real cook instead: at startup it cooks a unit cube, keeps its raw
 * sections, and each input is a list of edits to them, which the target
 * re-wraps with valid CRCs and loads with Model::LoadCooked. The model reader
 * then sees well-formed containers holding damaged sections.
 *
 * Input: records of [section u8][mode u8][offset u16][length u16][bytes...].
 * Mode 0 overwrites bytes at the offset (growing the section), mode 1 truncates
 * the section at the offset, mode 2 replaces it with the bytes.
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>
#include "UHE/AssetsManager/CookedFile.h"
#include "UHE/AssetsManager/PrimitiveGltf.h"
#include "UHE/Core/Log.h"
#include "UHE/Renderer3D/LoadModel.h"

namespace fs = std::filesystem;

namespace
{
struct Base
{
    u32 AssetType = 0;
    std::vector<u32> Ids;
    std::vector<std::vector<u8>> Sections;
    fs::path Scratch;
};

Base g_Base;

[[noreturn]] void Die(const std::string& why)
{
    std::fprintf(stderr, "fuzz_cooked_model: %s\n", why.c_str());
    std::abort();
}

UHE::RD3d::ModelLoadOptions NoGpu()
{
    UHE::RD3d::ModelLoadOptions options;
    options.upload = false;
    options.keepTextureSources = true;
    return options;
}
} // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***)
{
    UHE::Log::Init();
    UHE::Log::GetCoreLogger()->set_level(spdlog::level::off); // a refused input is the expected path
    const fs::path dir = fs::temp_directory_path() / ("uhe_fuzz_model_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const fs::path source = dir / "cube.gltf";
    std::string error;
    if (!UHE::WritePrimitiveGltf(source, UHE::PrimitiveShape::Cube, {0.8f, 0.6f, 0.4f, 1.0f}, 0.5f, &error))
        Die("cannot write the seed cube: " + error);
    UHE::RD3d::Model model;
    if (!model.loadModel(source, NoGpu()))
        Die("cannot load the seed cube");
    const fs::path cooked = dir / "cube.uhemodel";
    if (!model.SaveCooked(cooked, 0, &error))
        Die("cannot cook the seed cube: " + error);

    UHE::Cooked::Reader reader;
    if (!reader.Open(cooked, &error))
        Die("cannot reopen the seed cook: " + error);
    g_Base.AssetType = reader.AssetType();
    for (const UHE::Cooked::SectionInfo& info : reader.Sections())
    {
        std::vector<u8> raw;
        if (!reader.ReadSection(info.Id, raw, &error))
            Die("cannot read a seed section: " + error);
        g_Base.Ids.push_back(info.Id);
        g_Base.Sections.push_back(std::move(raw));
    }
    g_Base.Scratch = dir / "input.uhemodel";
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::vector<std::vector<u8>> sections = g_Base.Sections;
    std::size_t pos = 0;
    while (pos + 6 <= size)
    {
        const std::size_t index = data[pos] % sections.size();
        const int mode = data[pos + 1] % 3;
        const std::size_t offset = data[pos + 2] | (data[pos + 3] << 8);
        std::size_t length = data[pos + 4] | (data[pos + 5] << 8);
        pos += 6;
        length = std::min(length, size - pos);
        std::vector<u8>& section = sections[index];
        if (mode == 0)
        {
            const std::size_t at = std::min(offset, section.size());
            if (section.size() < at + length)
                section.resize(at + length);
            std::copy(data + pos, data + pos + length, section.begin() + static_cast<std::ptrdiff_t>(at));
        }
        else if (mode == 1)
            section.resize(std::min(offset, section.size()));
        else
            section.assign(data + pos, data + pos + length);
        pos += length;
    }

    UHE::Cooked::Writer writer;
    for (std::size_t i = 0; i < sections.size(); ++i)
        writer.AddSection(g_Base.Ids[i], sections[i], false);
    if (!writer.Write(g_Base.Scratch, g_Base.AssetType, 0))
        Die("cannot write the scratch file");
    UHE::RD3d::Model model;
    (void)model.LoadCooked(g_Base.Scratch, NoGpu());
    return 0;
}
