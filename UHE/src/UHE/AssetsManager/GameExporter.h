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
};

/// @brief What an export did, or why it stopped.
struct ExportReport
{
    bool Ok = false;
    std::vector<std::string> Errors;
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
 * - `assets/`: the engine's assets without shader sources (the runtime loads
 *   the compiled `.spv` and never compiles);
 * - `Project.uheproject` and `Assets/`: the project, which the runtime finds
 *   next to its executable;
 * - `.uhe-export`: marks the folder as an export.
 *
 * It never deletes anything. It refuses an output folder that has files in it
 * unless that folder is a previous export (it has the marker), whose files it
 * then overwrites.
 */
class UHE_API GameExporter
{
public:
    static ExportReport Export(const ProjectInfo& project, const ExportOptions& options);

    /// @brief The project name made safe for a file name (letters, digits, '-', '_', '.').
    [[nodiscard]] static std::string ExecutableName(const std::string& projectName);

    static constexpr const char* kMarkerName = ".uhe-export";
};

} // namespace UHE
