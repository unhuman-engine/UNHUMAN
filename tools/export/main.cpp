// uhe_export: writes a project out as a standalone game, without the editor.
//
//   uhe_export --project <dir> --out <dir> [--runtime-dir <dir>] [--engine-assets <dir>] [--all-assets]
//
// Only the assets the project's scenes and scripts reference are copied;
// --all-assets copies every file, for a game that builds asset paths at run time.
// The same GameExporter as the editor's File > Export Game. Defaults: the
// runtime from this tool's own folder (the build's bin directory) and the
// engine assets beside it (a packaged editor), else UHE_EDITOR/assets of the
// source tree it was built from.
#include <cstdio>
#include <filesystem>
#include <string>
#include "UHE/AssetsManager/GameExporter.h"
#include "UHE/AssetsManager/Project.h"

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    std::string project, out, runtimeDir, engineAssets;
    bool allAssets = false, usage = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string key = argv[i];
        const bool hasValue = i + 1 < argc;
        if (key == "--all-assets")
            allAssets = true;
        else if (key == "--project" && hasValue)
            project = argv[++i];
        else if (key == "--out" && hasValue)
            out = argv[++i];
        else if (key == "--runtime-dir" && hasValue)
            runtimeDir = argv[++i];
        else if (key == "--engine-assets" && hasValue)
            engineAssets = argv[++i];
        else
            usage = true;
    }
    if (usage || project.empty() || out.empty())
    {
        std::fprintf(stderr, "usage: uhe_export --project <dir> --out <dir> [--runtime-dir <dir>] "
                             "[--engine-assets <dir>] [--all-assets]\n");
        return 2;
    }

    UHE::ProjectInfo info;
    std::string error;
    if (!UHE::Project::Load(project, info, &error))
    {
        std::fprintf(stderr, "uhe_export: could not load '%s': %s\n", project.c_str(), error.c_str());
        return 1;
    }

    UHE::ExportOptions options;
    options.OutputDir = out;
    options.RuntimeDir = runtimeDir.empty() ? fs::absolute(argv[0]).parent_path() : fs::path(runtimeDir);
    options.EngineAssets = engineAssets;
    if (options.EngineAssets.empty())
    {
        // A packaged editor keeps the assets next to its tools; a build tree
        // has them in the source tree it was built from.
        std::error_code ec;
        const fs::path beside = options.RuntimeDir / "assets";
        options.EngineAssets = fs::is_directory(beside / "shaders", ec) ? beside : fs::path(UHE_ENGINE_ASSETS);
    }
    options.AllAssets = allAssets;
    const UHE::ExportReport report = UHE::GameExporter::Export(info, options);
    for (const std::string& message : report.Warnings)
        std::fprintf(stderr, "uhe_export: warning: %s\n", message.c_str());
    for (const std::string& message : report.Errors)
        std::fprintf(stderr, "uhe_export: %s\n", message.c_str());
    if (!report.Ok)
        return 1;
    std::printf("uhe_export: %s -> %s (%zu files, %ju MB)\n", info.Name.c_str(), report.Executable.string().c_str(),
                report.FilesCopied, static_cast<std::uintmax_t>(report.BytesCopied / (std::uintmax_t{1024} * 1024)));
    return 0;
}
