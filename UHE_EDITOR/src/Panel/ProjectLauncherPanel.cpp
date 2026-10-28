#include "ProjectLauncherPanel.h"

#include "EditorTheme.h"
#include "EditorUI.h"

#include "UHE/AssestsManager/VfsSystem.h"
#include "UHE/Utils/PlatfromUtils.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace UHE
{
    namespace
    {
        constexpr std::size_t kMaxRecent = 12;

        std::string FileNameOrPath(const std::filesystem::path& path)
        {
            const std::string name = path.filename().string();
            return name.empty() ? path.string() : name;
        }
    } // namespace

    std::filesystem::path ProjectLauncherPanel::SettingsPath()
    {
        // Same location the theme settings use: the working directory.
        return std::filesystem::current_path() / "editor_projects.ini";
    }

    ProjectLauncherPanel::ProjectLauncherPanel()
    {
        Load();
        m_Examples = ScanExamples();
        m_NewProjectParent = std::filesystem::current_path();
    }

    void ProjectLauncherPanel::Load()
    {
        m_Recent.clear();

        std::ifstream file(SettingsPath());
        if (!file)
            return;

        std::string line;
        while (std::getline(file, line))
        {
            constexpr const char* prefix = "Recent=";
            if (line.rfind(prefix, 0) != 0)
                continue;
            std::string value = line.substr(std::string(prefix).size());
            if (!value.empty())
                m_Recent.emplace_back(std::move(value));
        }
    }

    void ProjectLauncherPanel::Save() const
    {
        std::ofstream file(SettingsPath());
        if (!file)
            return; // a read-only directory must not break the launcher

        file << "# Recent projects, most recent first.\n";
        for (const std::filesystem::path& path : m_Recent)
            file << "Recent=" << path.string() << '\n';
    }

    void ProjectLauncherPanel::AddRecent(const ProjectInfo& info)
    {
        if (info.RootPath.empty())
            return;

        const std::filesystem::path root = info.RootPath;
        m_Recent.erase(std::remove_if(m_Recent.begin(), m_Recent.end(),
                                      [&](const std::filesystem::path& entry) {
                                          return entry.lexically_normal() == root.lexically_normal();
                                      }),
                       m_Recent.end());
        m_Recent.insert(m_Recent.begin(), root);
        if (m_Recent.size() > kMaxRecent)
            m_Recent.resize(kMaxRecent);
        Save();
    }

    void ProjectLauncherPanel::RemoveRecent(const std::filesystem::path& root)
    {
        m_Recent.erase(std::remove_if(m_Recent.begin(), m_Recent.end(),
                                      [&](const std::filesystem::path& entry) {
                                          return entry.lexically_normal() == root.lexically_normal();
                                      }),
                       m_Recent.end());
        Save();
    }

    std::vector<ProjectInfo> ProjectLauncherPanel::ScanExamples()
    {
        std::vector<ProjectInfo> examples;

        // <repo>/projects next to the editor's own folder, plus ./projects for
        // runs from a build directory. ScanFolder de-duplicates, so both spellings
        // of the same folder are harmless.
        const std::filesystem::path rootPath = FileSystem::Get().GetRootPath();
        if (!rootPath.empty())
            Project::ScanFolder(rootPath.parent_path() / "projects", examples);
        Project::ScanFolder(std::filesystem::current_path() / "projects", examples);

        std::sort(examples.begin(), examples.end(),
                  [](const ProjectInfo& a, const ProjectInfo& b) { return a.Name < b.Name; });
        return examples;
    }

    bool ProjectLauncherPanel::TryOpen(const std::filesystem::path& path, ProjectInfo& opened, bool& picked)
    {
        ProjectInfo info;
        std::string error;
        if (!Project::Load(path, info, &error))
        {
            SetMessage(error);
            return false;
        }

        opened = std::move(info);
        picked = true;
        return true;
    }

    bool ProjectLauncherPanel::OnImGuiRender(ProjectInfo& opened)
    {
        bool picked = false;

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
        ImGui::Begin("Projects", nullptr, flags);
        ImGui::PopStyleVar();

        ImGuiIO& io = ImGui::GetIO();
        ImFont* boldFont = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
        if (boldFont)
            ImGui::PushFont(boldFont);
        ImGui::TextColored(EditorTheme::Accent(), "UNHUMAN ENGINE");
        if (boldFont)
            ImGui::PopFont();
        ImGui::TextDisabled("Projects");
        ImGui::Separator();
        ImGui::Spacing();

        const float leftWidth = ImMax(ImGui::GetContentRegionAvail().x * 0.55f, 320.0f);
        ImGui::BeginChild("##RecentProjects", ImVec2(leftWidth, 0.0f), false);

        EditorUI::SectionHeader("Recent");
        if (m_Recent.empty())
        {
            ImGui::TextDisabled("No projects yet - create one or open an existing folder.");
        }
        else
        {
            DrawRecentList(opened, picked);
        }

        ImGui::Spacing();
        EditorUI::SectionHeader("Examples");
        if (m_Examples.empty())
        {
            ImGui::TextDisabled("No example projects found in ./projects.");
        }
        else
        {
            for (const ProjectInfo& example : m_Examples)
            {
                ImGui::PushID(example.RootPath.string().c_str());
                if (ImGui::Selectable(example.Name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    if (ImGui::IsMouseDoubleClicked(0))
                        TryOpen(example.RootPath, opened, picked);
                    else
                        SetMessage("Double-click to open " + example.Name + "  (" + example.RootPath.string() + ")");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s", example.Name.c_str(), example.RootPath.string().c_str());
                ImGui::PopID();
            }
        }

        ImGui::EndChild();

        ImGui::SameLine(0.0f, 24.0f);
        ImGui::BeginChild("##ProjectActions", ImVec2(0.0f, 0.0f), false);
        DrawActions(opened, picked);
        ImGui::EndChild();

        if (!m_Message.empty())
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextWrapped("%s", m_Message.c_str());
        }

        DrawNewProjectDialog(opened, picked);
        ImGui::End();

        return picked;
    }

    void ProjectLauncherPanel::DrawRecentList(ProjectInfo& opened, bool& picked)
    {
        for (const std::filesystem::path& root : m_Recent)
        {
            std::error_code ec;
            const bool exists = Project::IsProject(root);
            const std::string label = FileNameOrPath(root) + (exists ? "" : "  (missing)");

            ImGui::PushID(root.string().c_str());
            if (!exists)
                ImGui::BeginDisabled();

            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick))
            {
                if (ImGui::IsMouseDoubleClicked(0))
                    TryOpen(root, opened, picked);
                else
                    SetMessage("Double-click to open " + root.string());
            }

            if (!exists)
                ImGui::EndDisabled();

            if (ImGui::IsItemHovered() && exists)
                ImGui::SetTooltip("%s", root.string().c_str());

            if (ImGui::BeginPopupContextItem("RecentContext"))
            {
                if (ImGui::MenuItem("Remove from list"))
                {
                    RemoveRecent(root);
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }

    void ProjectLauncherPanel::DrawActions(ProjectInfo& opened, bool& picked)
    {
        EditorUI::SectionHeader("Start");

        if (ImGui::Button("New Project...", ImVec2(-1.0f, 32.0f)))
        {
            m_Message.clear();
            m_OpenNewProjectPopup = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Create a folder with a project manifest and an Assets folder");

        ImGui::Spacing();
        if (ImGui::Button("Open Project...", ImVec2(-1.0f, 32.0f)))
        {
            const std::string folder = FileDialogs::SelectFolder("Open Project", std::filesystem::current_path().string());
            if (!folder.empty())
                TryOpen(folder, opened, picked);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Open a folder that contains a Project.uheproject manifest");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("A project owns the assets: its Assets folder is mounted as\n"
                            "game:, so scenes and components can use project-relative paths.");
    }

    void ProjectLauncherPanel::DrawNewProjectDialog(ProjectInfo& opened, bool& picked)
    {
        if (m_OpenNewProjectPopup)
        {
            ImGui::OpenPopup("New Project");
            m_OpenNewProjectPopup = false;
        }

        ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;

        ImGui::TextDisabled("Name");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##ProjectName", m_NewProjectName, sizeof(m_NewProjectName));

        ImGui::Spacing();
        ImGui::TextDisabled("Folder");
        const std::string parent = m_NewProjectParent.string();
        ImGui::TextWrapped("%s", parent.c_str());
        if (ImGui::Button("Choose Folder..."))
        {
            const std::string folder = FileDialogs::SelectFolder("Project Folder", parent);
            if (!folder.empty())
                m_NewProjectParent = folder;
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Creates %s/%s with Project.uheproject and Assets/",
                            m_NewProjectParent.filename().string().c_str(), m_NewProjectName);

        ImGui::Separator();
        const bool nameEmpty = m_NewProjectName[0] == '\0';
        ImGui::BeginDisabled(nameEmpty);
        if (ImGui::Button("Create", ImVec2(120.0f, 0.0f)))
        {
            ProjectInfo created;
            std::string error;
            if (Project::Create(m_NewProjectParent / m_NewProjectName, m_NewProjectName, created, &error))
            {
                opened = std::move(created);
                picked = true;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                SetMessage(error);
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }
} // namespace UHE
