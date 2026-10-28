#pragma once

#include "UHE/AssestsManager/Project.h"

#include <filesystem>
#include <string>
#include <vector>

namespace UHE
{
    /**
     * @brief Startup screen: pick, create or open a project before the editor
     * proper appears (the editor is useless without an asset root).
     *
     * Owns the recent-project list, persisted next to the editor's other
     * settings (`editor_projects.ini` in the working directory, matching
     * EditorTheme's pattern).
     */
    class ProjectLauncherPanel
    {
    public:
        ProjectLauncherPanel();

        // Draws the launcher. Returns true (and fills `opened`) on the frame the
        // user picked, created or opened a project.
        bool OnImGuiRender(ProjectInfo& opened);

        // Records a successfully opened project at the top of the recent list.
        void AddRecent(const ProjectInfo& info);
        void RemoveRecent(const std::filesystem::path& root);

        [[nodiscard]] const std::vector<std::filesystem::path>& GetRecent() const { return m_Recent; }
        [[nodiscard]] const std::string& GetMessage() const { return m_Message; }
        void SetMessage(std::string message) { m_Message = std::move(message); }

        // Projects shipped with the repository (`<repo>/projects/*`), so a fresh
        // checkout has something to open.
        [[nodiscard]] static std::vector<ProjectInfo> ScanExamples();

        void Load();
        void Save() const;
        [[nodiscard]] static std::filesystem::path SettingsPath();

    private:
        void DrawRecentList(ProjectInfo& opened, bool& picked);
        void DrawActions(ProjectInfo& opened, bool& picked);
        void DrawNewProjectDialog(ProjectInfo& opened, bool& picked);
        bool TryOpen(const std::filesystem::path& path, ProjectInfo& opened, bool& picked);

        std::vector<std::filesystem::path> m_Recent;
        std::vector<ProjectInfo> m_Examples;
        std::string m_Message;

        // New-project dialog state
        bool m_OpenNewProjectPopup = false;
        char m_NewProjectName[128] = "MyProject";
        std::filesystem::path m_NewProjectParent;
    };
} // namespace UHE
