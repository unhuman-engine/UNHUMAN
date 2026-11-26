#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "UHE/AssetsManager/Project.h"

namespace UHE
{
/**
 * @brief The project manager: pick, create or open a project before the
 * editor proper appears (the editor is useless without an asset root).
 *
 * Recent projects and the example projects are listed with their name, folder
 * and start scene; one click opens. A recent project that is gone stays
 * listed as missing until it is removed. New projects are created from the
 * panel on the right, with their name checked as it is typed, and can start
 * with a scene (ground, cube, light and camera) instead of nothing.
 *
 * Settings (recent list, last folder) are kept in `editor_projects.ini` next
 * to the editor's assets.
 */
class ProjectLauncherPanel
{
public:
    /// @brief What the user chose this frame.
    struct Result
    {
        ProjectInfo Project;
        bool Created = false;      ///< a new project, just written
        bool StarterScene = false; ///< it asked for the starter scene
    };

    ProjectLauncherPanel();

    /// @brief Draws the manager; the result on the frame a project was opened or created.
    std::optional<Result> OnImGuiRender();

    /// @brief Records a successfully opened project at the top of the recent list.
    void AddRecent(const ProjectInfo& info);
    void RemoveRecent(const std::filesystem::path& root);
    /// @brief Rereads every listed project's manifest (names, start scenes, which are missing).
    void Refresh();

    [[nodiscard]] const std::vector<std::filesystem::path>& GetRecent() const { return m_Recent; }
    [[nodiscard]] const std::string& GetMessage() const { return m_Message; }
    /// @brief Shows \p message above the lists; \p error colours it as a failure.
    void SetMessage(std::string message, bool error = true)
    {
        m_Message = std::move(message);
        m_MessageIsError = error;
    }

    /// @brief Projects shipped with the repository (`<repo>/projects/*`), so a fresh checkout has something to open.
    [[nodiscard]] static std::vector<ProjectInfo> ScanExamples();

    void Load();
    void Save() const;
    [[nodiscard]] static std::filesystem::path SettingsPath();

private:
    /// One row of a list, read from its manifest by Refresh().
    struct Entry
    {
        std::filesystem::path Root;
        std::string Name;       ///< the manifest's name; the folder name when missing
        std::string StartScene; ///< empty when unset
        bool Exists = false;
    };

    void DrawHeader();
    void DrawMessage();
    void DrawLists(std::optional<Result>& result);
    /// @return true when the row asked to be removed (recent list only).
    bool DrawEntry(const Entry& entry, bool removable, std::optional<Result>& result);
    void DrawNewProject(std::optional<Result>& result);
    void DrawOpenExisting(std::optional<Result>& result);
    void TryOpen(const std::filesystem::path& path, std::optional<Result>& result);
    [[nodiscard]] bool Matches(const Entry& entry) const;
    [[nodiscard]] static std::filesystem::path DefaultLocation(const std::vector<std::filesystem::path>& recent);

    std::vector<std::filesystem::path> m_Recent;
    std::vector<Entry> m_RecentEntries;
    std::vector<Entry> m_ExampleEntries;
    std::string m_Message;
    bool m_MessageIsError = true;
    char m_Filter[128] = "";

    // New project form
    char m_NewProjectName[128] = "MyGame";
    std::filesystem::path m_NewProjectParent;
    bool m_NewProjectStarter = true;
};
} // namespace UHE
