// Offline shader compiler: Slang in, `.spv` out, at build time.
//
// The engine used to compile every shader with the Slang library while it was
// starting up: a session, a module load and a link per entry point, on the
// critical path of every launch. This tool does that work once, next to the
// sources, so the runtime only reads bytes back (`ShaderCache`), and the
// artifact is there for the pipeline builder to reflect on.
//
//   uhe_shaderc --shaders <dir> --source <file> [--source <file>...] [--force]
//
// Compiles each `--source` once per variant declared in `<dir>/variants.txt` and
// writes `<stem>.<stage>.<variant>.spv` beside it. The source list is explicit
// because the directory also holds library fragments and work-in-progress files
// with no engine entry point -- `PBRBasic.slang` is a BRDF library, and asking it
// for `vertexMain` is a question with no answer, not a broken shader.
//
// Up to date is decided by mtime, so a build that changed nothing compiles
// nothing; `--force` recompiles regardless. Exits non-zero when a source fails to
// compile, which fails the build rather than the first frame.
//
// It links the same `SlangCompiler` the engine's hot-reload path uses, so the
// artifact and a runtime compile cannot disagree about a session setting.

#include <filesystem>
#include <iostream>
#include <string>
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer/SlangCompiler.h"

namespace
{
constexpr UHE::RHI::ShaderStage kStages[] = {UHE::RHI::ShaderStage::Vertex, UHE::RHI::ShaderStage::Fragment,
                                             UHE::RHI::ShaderStage::Compute};

std::filesystem::file_time_type ModificationTime(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : time;
}

/// True when every artifact this variant produces exists and is newer than the
/// source. The stage set is not known without compiling, so "the artifacts that
/// are there are current" is the test -- a source that gained an entry point is
/// newer than what is on disk and gets recompiled.
bool UpToDate(const std::filesystem::path& source, const UHE::ShaderVariant& variant)
{
    const auto sourceTime = ModificationTime(source);
    bool anyArtifact = false;

    for (const UHE::RHI::ShaderStage stage : kStages)
    {
        const std::filesystem::path path = UHE::ShaderArtifactPath(source, variant.Name, stage);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
        {
            continue;
        }
        anyArtifact = true;
        if (sourceTime > ModificationTime(path))
        {
            return false;
        }
    }
    return anyArtifact;
}

void PrintUsage()
{
    std::cerr << "usage: uhe_shaderc --shaders <dir> --source <file> [--source <file>...] [--force]\n";
}
} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path shaderDir;
    std::vector<std::filesystem::path> sources;
    bool force = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--shaders" && i + 1 < argc)
        {
            shaderDir = argv[++i];
        }
        else if (argument == "--source" && i + 1 < argc)
        {
            sources.emplace_back(argv[++i]);
        }
        else if (argument == "--force")
        {
            force = true;
        }
        else
        {
            std::cerr << "uhe_shaderc: unexpected argument '" << argument << "'\n";
            PrintUsage();
            return 2;
        }
    }

    if (shaderDir.empty() || sources.empty())
    {
        PrintUsage();
        return 2;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(shaderDir, ec))
    {
        std::cerr << "uhe_shaderc: no shader directory at '" << shaderDir.string() << "'\n";
        return 2;
    }

    const std::vector<UHE::ShaderVariant> variants = UHE::LoadShaderVariants(shaderDir);

    std::cout << "uhe_shaderc: " << sources.size() << " source(s), " << variants.size() << " variant(s)\n";

    int compiled = 0;
    int skipped = 0;
    int failed = 0;

    for (const std::filesystem::path& source : sources)
    {
        if (!std::filesystem::is_regular_file(source, ec))
        {
            std::cerr << "uhe_shaderc: no shader source at '" << source.string() << "'\n";
            ++failed;
            continue;
        }

        for (const UHE::ShaderVariant& variant : variants)
        {
            if (!force && UpToDate(source, variant))
            {
                ++skipped;
                continue;
            }

            const auto stages = UHE::SlangCompiler::CompileToSPIRV(source.string(), variant.Defines);
            if (stages.empty())
            {
                std::cerr << "uhe_shaderc: '" << source.string() << "' [" << variant.Name << "] produced no SPIR-V\n";
                ++failed;
                continue;
            }

            std::cout << "  " << source.filename().string() << " [" << variant.Name << "]";
            for (const UHE::RHI::ShaderStage stage : kStages)
            {
                const auto stageIt = stages.find(stage);
                if (stageIt == stages.end())
                {
                    continue;
                }

                const std::filesystem::path path = UHE::ShaderArtifactPath(source, variant.Name, stage);
                if (!UHE::WriteShaderArtifact(path, stageIt->second))
                {
                    std::cerr << "\nuhe_shaderc: could not write '" << path.string() << "'\n";
                    ++failed;
                    continue;
                }
                std::cout << " " << path.filename().string();
            }
            std::cout << "\n";
            ++compiled;
        }
    }

    std::cout << "uhe_shaderc: " << compiled << " compiled, " << skipped << " up to date, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
