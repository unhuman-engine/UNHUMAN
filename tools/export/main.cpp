// uhe_export: writes a project out as a standalone game, without the editor.
//
//   uhe_export --project <dir> --out <dir> [--runtime-dir <dir>] [--engine-assets <dir>] [--all-assets]
//              [--no-cook] [--platform desktop|mobile] [--texture-codec keep|auto|etc1s|uastc]
//              [--max-texture-size <pixels>] [--threads <n>]
//
// Models ship cooked (`<model>.uhemodel`, Model::SaveCooked) in place of the
// glTF and the files only it uses, so the game never parses glTF; --no-cook
// ships them as they are. A cook stores its images as KTX2 (tools/texcook);
// --platform picks the texture settings for a platform (mobile: at most 2048
// pixels), and the two texture flags override them.
// Only the assets the project's scenes and scripts reference are copied;
// --all-assets copies every file, for a game that builds asset paths at run time.
// The same GameExporter as the editor's File > Export Game. Defaults: the
// runtime from this tool's own folder (the build's bin directory) and the
// engine assets beside it (a packaged editor), else UHE_EDITOR/assets of the
// source tree it was built from.
#include <cstdio>
#include <filesystem>
#include <string>
#include "ModelCookOptions.h"
#include "UHE/AssetsManager/GameExporter.h"
#include "UHE/AssetsManager/Project.h"
#include "UHE/Audio/CookedAudio.h"
#include "UHE/Renderer3D/LoadModel.h"

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    std::string project, out, runtimeDir, engineAssets, platform = "desktop", codec, maxSize, threads;
    bool allAssets = false, usage = false, cook = true;
    for (int i = 1; i < argc; ++i)
    {
        const std::string key = argv[i];
        const bool hasValue = i + 1 < argc;
        if (key == "--all-assets")
            allAssets = true;
        else if (key == "--no-cook")
            cook = false;
        else if (key == "--project" && hasValue)
            project = argv[++i];
        else if (key == "--out" && hasValue)
            out = argv[++i];
        else if (key == "--runtime-dir" && hasValue)
            runtimeDir = argv[++i];
        else if (key == "--engine-assets" && hasValue)
            engineAssets = argv[++i];
        else if (key == "--platform" && hasValue)
            platform = argv[++i];
        else if (key == "--texture-codec" && hasValue)
            codec = argv[++i];
        else if (key == "--max-texture-size" && hasValue)
            maxSize = argv[++i];
        else if (key == "--threads" && hasValue)
            threads = argv[++i];
        else
            usage = true;
    }
    UHE::Cook::TextureCookOptions textures;
    if (!UHE::Cook::TextureOptionsForPlatform(platform, textures))
    {
        std::fprintf(stderr, "uhe_export: unknown platform '%s' (desktop, mobile)\n", platform.c_str());
        return 2;
    }
    if (!codec.empty() && !UHE::Cook::ParseTextureCodec(codec, textures.Codec))
    {
        std::fprintf(stderr, "uhe_export: unknown texture codec '%s' (keep, auto, etc1s, uastc)\n", codec.c_str());
        return 2;
    }
    if (!maxSize.empty())
        textures.MaxSize = static_cast<std::uint32_t>(std::stoul(maxSize));
    if (!threads.empty())
        textures.Threads = static_cast<std::uint32_t>(std::stoul(threads));
    if (usage || project.empty() || out.empty())
    {
        std::fprintf(stderr, "usage: uhe_export --project <dir> --out <dir> [--runtime-dir <dir>] "
                             "[--engine-assets <dir>] [--all-assets] [--no-cook] [--platform desktop|mobile]\n"
                             "                  [--texture-codec keep|auto|etc1s|uastc] [--max-texture-size <pixels>]\n"
                             "                  [--threads <n>]\n");
        return 2;
    }

    UHE::ProjectInfo info;
    std::string error;
    if (!UHE::Project::Load(project, info, &error))
    {
        std::fprintf(stderr, "uhe_export: could not load '%s': %s\n", project.c_str(), error.c_str());
        return 1;
    }

    // Encoded images are kept per project: a second export encodes only what changed.
    textures.CacheDir = info.RootPath / ".uhecache" / "textures";
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
    options.Platform = platform;
    if (cook)
    {
        const UHE::RD3d::CookOptions cookOptions = UHE::Cook::MakeModelCookOptions(textures);
        options.CookModel = [cookOptions](const fs::path& source, const fs::path& cooked, std::string& why)
        { return UHE::RD3d::CookModelFile(source, cooked, why, cookOptions); };
        if (textures.Codec != UHE::Cook::TextureCodec::Keep)
            options.CookTexture = [textures](const fs::path& source, const fs::path& cooked, std::string& why)
            { return UHE::Cook::CookTextureFile(source, cooked, textures, why); };
        options.CookAudio = UHE::CookAudioFile;
    }
    const UHE::ExportReport report = UHE::GameExporter::Export(info, options);
    for (const std::string& message : report.Warnings)
        std::fprintf(stderr, "uhe_export: warning: %s\n", message.c_str());
    for (const std::string& message : report.Errors)
        std::fprintf(stderr, "uhe_export: %s\n", message.c_str());
    if (!report.Ok)
        return 1;
    std::printf("uhe_export: %s -> %s (%zu files, %zu models, %zu images and %zu sounds cooked, %ju MB)\n",
                info.Name.c_str(), report.Executable.string().c_str(), report.FilesCopied, report.ModelsCooked,
                report.ImagesCooked, report.SoundsCooked,
                static_cast<std::uintmax_t>(report.BytesCopied / (std::uintmax_t{1024} * 1024)));
    return 0;
}
