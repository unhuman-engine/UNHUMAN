#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
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
    /**
     * Cooks the glTF/GLB model at the first path into the cooked file at the
     * second (Model::SaveCooked); false with a reason when it cannot. When
     * set, each referenced model ships as `<model>.uhemodel` instead of the
     * glTF and the buffers and images only it uses, and the game never parses
     * glTF. Unset (tests, tools without a renderer): models ship as they are.
     */
    std::function<bool(const std::filesystem::path&, const std::filesystem::path&, std::string&)> CookModel;
    /// The platform the game is for, recorded in its AssetManifest.yaml (desktop, mobile).
    std::string Platform = "desktop";
    /**
     * Refresh `<project>/AssetRegistry.yaml` (stable IDs for every file under
     * Assets/) before writing the game's manifest. Off only for a read-only
     * project; the manifest's IDs are then the registry's as it stands.
     */
    bool UpdateRegistry = true;
};

/// @brief The files a game reads, found from its scenes and scripts (GameExporter::CollectAssets).
struct AssetManifest
{
    std::vector<std::filesystem::path> ProjectFiles; ///< relative to the project's Assets/, sorted
    std::vector<std::filesystem::path> EngineFiles;  ///< relative to the engine asset folder, sorted
    std::vector<std::string> Warnings;               ///< references the game will not be able to load
    /// Per model (.gltf/.glb, as listed above): its buffers and images that nothing else references.
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> ProjectModelFiles;
    std::map<std::filesystem::path, std::vector<std::filesystem::path>> EngineModelFiles;
};

/// @brief What an export did, or why it stopped.
struct ExportReport
{
    bool Ok = false;
    std::vector<std::string> Errors;
    std::vector<std::string> Warnings; ///< the export finished, but these references will not load
    std::size_t FilesCopied = 0;
    std::uintmax_t BytesCopied = 0;
    std::size_t ModelsCooked = 0;     ///< shipped as .uhemodel (ExportOptions::CookModel)
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

    /**
     * @brief Writes `<out>/Assets/AssetManifest.yaml`: every shipped file with its stable ID (issue #45).
     *
     * Project files take their IDs from `<project>/AssetRegistry.yaml`, which
     * this refreshes first (ExportOptions::UpdateRegistry); engine files get
     * an ID derived from their path. A cooked model is listed under the glTF
     * path scenes name, with the `.uhemodel` as the file that stores it.
     */
    static void WriteAssetManifest(const ProjectInfo& project, const ExportOptions& options,
                                   const std::filesystem::path& out, ExportReport& report);

    static constexpr const char* kMarkerName = ".uhe-export";
    /// Written by uhe_shaderc next to the sources it compiles (tools/shaderc/main.cpp).
    static constexpr const char* kShaderManifestName = "shaders.manifest";
};

} // namespace UHE
