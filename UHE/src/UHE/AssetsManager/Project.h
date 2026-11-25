#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"
#include "UHE/Core/Window.h"

namespace UHE
{
/**
 * @brief One project: a directory that owns a game's assets.
 *
 * A project is a directory holding a YAML manifest (`Project.uheproject`) and
 * an `Assets/` folder. Opening one mounts `game:` onto that asset folder, so
 * scenes, sprites and models reference assets with project-relative paths,
 * while the engine's own assets (shaders, icons, fonts) stay reachable
 * through the `engine:` mount. Nothing here touches the GPU or ImGui.
 */
/**
 * @brief The window an exported game opens (the `Window` block of the manifest).
 *
 * The runtime creates its window from these; the player can still override
 * the mode and size on the command line (-windowed, -borderless, -fullscreen,
 * -width, -height).
 */
struct UHE_API ProjectWindowSettings
{
    static constexpr u32 kMinSize = 320;
    static constexpr u32 kMaxSize = 16384;

    std::string Title; ///< empty: the project's name
    u32 Width = 1280;  ///< windowed size, within [kMinSize, kMaxSize]
    u32 Height = 720;
    WindowMode Mode = WindowMode::Windowed; ///< Borderless and Fullscreen use the monitor's desktop resolution
    bool VSync = true;
};

/// @brief "Windowed", "Borderless" or "Fullscreen".
UHE_API const char* WindowModeName(WindowMode mode);
/// @brief The mode named \p name (any case); \p fallback when it names none.
UHE_API WindowMode ParseWindowMode(const std::string& name, WindowMode fallback);

struct UHE_API ProjectInfo
{
    std::string Name;                // display name
    std::filesystem::path RootPath;  // project directory (manifest's parent)
    std::filesystem::path AssetPath; // RootPath / "Assets"
    std::string StartScene;          // optional, path relative to RootPath
    std::string EngineVersion;       // informational, written on create/save
    ProjectWindowSettings Window;    // the exported game's window

    [[nodiscard]] bool IsValid() const { return !Name.empty() && !RootPath.empty(); }
};

class UHE_API Project
{
public:
    static constexpr const char* kManifestName = "Project.uheproject";
    static constexpr const char* kAssetFolderName = "Assets";

    // Accepts either a project directory or the manifest path itself.
    [[nodiscard]] static std::filesystem::path ManifestPath(const std::filesystem::path& path);
    [[nodiscard]] static bool IsProject(const std::filesystem::path& path);

    // Writes <root>/Project.uheproject and creates <root>/Assets. Fails when a
    // manifest is already there or the directories cannot be created.
    static bool Create(const std::filesystem::path& root, const std::string& name, ProjectInfo& out,
                       std::string* error = nullptr);

    // Reads a manifest. Does not create or modify anything on disk.
    static bool Load(const std::filesystem::path& path, ProjectInfo& out, std::string* error = nullptr);

    static bool Save(const ProjectInfo& info, std::string* error = nullptr);

    // Points the VFS at this project: "game:" -> <project>/Assets, plus
    // "engine:" -> engineAssetPath (the asset folder shipped next to the
    // binary) so engine-side assets keep resolving. Existing mounts are
    // replaced in place, which keeps the project ahead of the engine folder.
    static void Mount(const ProjectInfo& info, const std::filesystem::path& engineAssetPath);

    // Absolute path of the project's start scene; empty when unset.
    [[nodiscard]] static std::filesystem::path StartScenePath(const ProjectInfo& info);

    // Creates <project>/Assets when it is missing (used when opening a
    // project whose asset folder was removed or never committed).
    static bool EnsureAssetFolder(const ProjectInfo& info, std::string* error = nullptr);

    // Appends every project directly inside `folder` to `out`, skipping
    // entries already there (compared by canonical root path, so scanning the
    // same folder through two different spellings adds nothing twice) and
    // entries that are not projects. Returns the number appended.
    static std::size_t ScanFolder(const std::filesystem::path& folder, std::vector<ProjectInfo>& out);
};
} // namespace UHE
