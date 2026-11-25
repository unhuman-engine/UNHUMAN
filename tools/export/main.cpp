// uhe_export: writes a project out as a standalone game, without the editor.
//
//   uhe_export --project <dir> --out <dir> [--runtime-dir <dir>] [--engine-assets <dir>]
//
// The same GameExporter as the editor's File > Export Game. Defaults: the
// runtime from this tool's own folder (the build's bin directory) and the
// engine assets from UHE_EDITOR/assets of the source tree it was built from.
#include <cstdio>
#include <filesystem>
#include <string>
#include "UHE/AssetsManager/GameExporter.h"
#include "UHE/AssetsManager/Project.h"

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    std::string project, out, runtimeDir, engineAssets;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string key = argv[i];
        if (key == "--project")
            project = argv[i + 1];
        else if (key == "--out")
            out = argv[i + 1];
        else if (key == "--runtime-dir")
            runtimeDir = argv[i + 1];
        else if (key == "--engine-assets")
            engineAssets = argv[i + 1];
    }
    if (project.empty() || out.empty())
    {
        std::fprintf(stderr, "usage: uhe_export --project <dir> --out <dir> [--runtime-dir <dir>] "
                             "[--engine-assets <dir>]\n");
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
    options.EngineAssets = engineAssets.empty() ? fs::path(UHE_ENGINE_ASSETS) : fs::path(engineAssets);
    const UHE::ExportReport report = UHE::GameExporter::Export(info, options);
    for (const std::string& message : report.Errors)
        std::fprintf(stderr, "uhe_export: %s\n", message.c_str());
    if (!report.Ok)
        return 1;
    std::printf("uhe_export: %s -> %s (%zu files, %ju MB)\n", info.Name.c_str(), report.Executable.string().c_str(),
                report.FilesCopied, static_cast<std::uintmax_t>(report.BytesCopied / (1024 * 1024)));
    return 0;
}
