#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "UHE/AssetsManager/Project.h"
#include "UHE/Core/Core.h"

namespace UHE
{

/// @brief Where an export reads from and writes to.
struct ExportOptions
{
    std::filesystem::path OutputDir;         ///< the game folder to create; empty, absent or a previous export
    std::filesystem::path RuntimeDir;        ///< folder holding the runtime executable and its shared libraries
    std::string RuntimeName = "UHE_RUNTIME"; ///< the runtime executable's file name (".exe" added on Windows)
    std::filesystem::path EngineAssets;      ///< the engine's asset folder (shaders, fonts, icons, engine models)
    /// Copy every project and engine asset instead of what the scenes and
    /// scripts reference: for a game that builds asset paths at run time.
    bool AllAssets = false;
};

/// @brief The files a game reads, found from its scenes and scripts (GameExporter::CollectAssets).
struct AssetManifest
{
    std::vector<std::filesystem::path> ProjectFiles; ///< relative to the project's Assets/, sorted
    std::vector<std::filesystem::path> EngineFiles;  ///< relative to the engine asset folder, sorted
    std::vector<std::string> Warnings;               ///< references the game will not be able to load
};

/// @brief What an export did, or why it stopped.
struct ExportReport
{
    bool Ok = false;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings; ///< the export finished, but these references will not load
    std::size_t FilesCopied = 0;
    std::uintmax_t BytesCopied = 0;
    std::filesystem::path Executable; ///< the game's executable, when Ok
};

/**
 * @brief Writes a project out as a game that runs without the editor or the source tree.
 *
 * The folder it produces:
 * - `<Project name>[.exe]`: the runtime (UHE_RUNTIME), renamed, with every
 *   shared library next to it (Linux binaries find them through $ORIGIN);
 * - `assets/`: the engine assets the runtime itself loads (compiled shaders
 *   without their sources, icons, the default fonts) and those the scenes use;
 * - `Project.uheproject` and `Assets/`: the manifest (written from the
 *   ProjectInfo given, saved or not) and the project assets
 *   the game references (CollectAssets), which the runtime finds next to its
 *   executable;
 * - `.uhe-export`: marks the folder as an export.
 *
 * Shared libraries Slang loads only to compile (its LLVM, glslang and GLSL
 * modules, gfx) are left out: a game never compiles a shader.
 *
 * It never deletes anything. It refuses an output folder that has files in it
 * unless that folder is a previous export (it has the marker), whose files it
 * then overwrites.
 */
class UHE_API GameExporter
{
public:
    static ExportReport Export(const ProjectInfo& project, const ExportOptions& options);

    /**
     * @brief Finds the files a game made from these folders reads.
     *
     * Every scene (`.unhuman`) and script (`.lua`) in the project is kept, and
     * so is everything they name: each scene value and each script string that
     * resolves to a file the way ResolveAssetPath does (project first, then
     * engine; `game:` and `engine:` prefixes pick one). A glTF brings the
     * buffers and images it points at, and a licence file comes with every
     * asset in its folder or above. The engine files the runtime itself loads
     * are always in. A reference that looks like an asset but resolves to
     * nothing, or that is an absolute path, is a warning.
     */
    [[nodiscard]] static AssetManifest CollectAssets(const std::filesystem::path& projectAssets,
                                                     const std::filesystem::path& engineAssets,
                                                     const std::vector<std::filesystem::path>& extraScenes = {});

    /// @brief Every .slang under \p engineAssets with no compiled `.spv` beside it
    ///        (relative paths); an export with any is refused, a game cannot compile.
    [[nodiscard]] static std::vector<std::string> MissingShaderArtifacts(const std::filesystem::path& engineAssets);

    /// @brief The project name made safe for a file name (letters, digits, '-', '_', '.').
    [[nodiscard]] static std::string ExecutableName(const std::string& projectName);

    static constexpr const char* kMarkerName = ".uhe-export";
    /// Written by uhe_shaderc next to the sources it compiles (tools/shaderc/main.cpp).
    static constexpr const char* kShaderManifestName = "shaders.manifest";
};

} // namespace UHE
