#include "ProjectLauncherPanel.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include "EditorTheme.h"
#include "EditorUI.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Utils/PlatfromUtils.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace UHE
{
namespace
{
constexpr std::size_t kMaxRecent = 16;

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// The user's home folder, or nothing when the environment does not say.
std::filesystem::path HomeFolder()
{
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home && *home ? std::filesystem::path(home) : std::filesystem::path();
}

/// Text drawn dimmed, with an ellipsis when it does not fit \p width.
void DimText(const std::string& text, float width)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float lineHeight = ImGui::GetTextLineHeight();
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + width, pos.y + lineHeight), pos.x + width,
                              text.c_str(), nullptr, nullptr);
    ImGui::Dummy(ImVec2(width, lineHeight));
    ImGui::PopStyleColor();
}
} // namespace

std::filesystem::path ProjectLauncherPanel::SettingsPath()
{
    // The engine's own folder, next to its assets and logs, rather than the
    // working directory: the editor is started from a terminal, from the
    // build directory and from a file manager, and a recent-project list
    // that changes with the launch directory is not a recent-project list.
    const std::filesystem::path root = FileSystem::Get().GetRootPath();
    return (root.empty() ? std::filesystem::current_path() : root) / "editor_projects.ini";
}

ProjectLauncherPanel::ProjectLauncherPanel()
{
    Load();
    if (m_NewProjectParent.empty())
        m_NewProjectParent = DefaultLocation(m_Recent);
    Refresh();
}

std::filesystem::path ProjectLauncherPanel::DefaultLocation(const std::vector<std::filesystem::path>& recent)
{
    // Beside the most recent project that still exists, else the home folder.
    for (const std::filesystem::path& root : recent)
        if (Project::IsProject(root))
            return root.parent_path();
    const std::filesystem::path home = HomeFolder();
    return home.empty() ? std::filesystem::current_path() : home;
}

void ProjectLauncherPanel::Load()
{
    m_Recent.clear();

    std::ifstream file(SettingsPath());
    // The list used to live in the working directory, so a session that
    // still has one there is read once and written back to the new place
    // the next time a project is opened.
    if (!file)
        file.open(std::filesystem::current_path() / "editor_projects.ini");
    if (!file)
        return;

    std::string line;
    while (std::getline(file, line))
    {
        const auto value = [&](const char* prefix) -> std::optional<std::string>
        {
            const std::string key(prefix);
            if (line.rfind(key, 0) != 0 || line.size() == key.size())
                return std::nullopt;
            return line.substr(key.size());
        };
        if (const auto recent = value("Recent="))
        {
            // Older lists hold relative and trailing-slash spellings of the
            // same folder; one spelling each, first one wins.
            const std::filesystem::path root = Project::NormalizeRoot(*recent);
            if (std::find(m_Recent.begin(), m_Recent.end(), root) == m_Recent.end())
                m_Recent.push_back(root);
        }
        else if (const auto location = value("Location="))
        {
            m_NewProjectParent = *location;
        }
    }
    if (m_Recent.size() > kMaxRecent)
        m_Recent.resize(kMaxRecent);
}

void ProjectLauncherPanel::Save() const
{
    std::ofstream file(SettingsPath());
    if (!file)
        return; // a read-only directory must not break the launcher

    file << "# Recent projects, most recent first.\n";
    for (const std::filesystem::path& path : m_Recent)
        file << "Recent=" << path.string() << '\n';
    if (!m_NewProjectParent.empty())
        file << "Location=" << m_NewProjectParent.string() << '\n';
}

void ProjectLauncherPanel::AddRecent(const ProjectInfo& info)
{
    if (info.RootPath.empty())
        return;
    const std::filesystem::path root = Project::NormalizeRoot(info.RootPath);
    std::erase(m_Recent, root);
    m_Recent.insert(m_Recent.begin(), root);
    if (m_Recent.size() > kMaxRecent)
        m_Recent.resize(kMaxRecent);
    Save();
    Refresh();
}

void ProjectLauncherPanel::RemoveRecent(const std::filesystem::path& root)
{
    std::erase(m_Recent, Project::NormalizeRoot(root));
    Save();
    Refresh();
}

void ProjectLauncherPanel::Refresh()
{
    const auto read = [](const std::filesystem::path& root)
    {
        Entry entry;
        entry.Root = root;
        entry.Name = root.filename().string();
        ProjectInfo info;
        if (Project::IsProject(root) && Project::Load(root, info))
        {
            entry.Exists = true;
            entry.Name = info.Name;
            entry.StartScene = info.StartScene;
        }
        return entry;
    };
    m_RecentEntries.clear();
    for (const std::filesystem::path& root : m_Recent)
        m_RecentEntries.push_back(read(root));
    m_ExampleEntries.clear();
    for (const ProjectInfo& example : ScanExamples())
        m_ExampleEntries.push_back(read(Project::NormalizeRoot(example.RootPath)));
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
    // A packaged editor keeps them beside itself.
    if (!rootPath.empty())
        Project::ScanFolder(rootPath / "projects", examples);
    Project::ScanFolder(std::filesystem::current_path() / "projects", examples);

    std::sort(examples.begin(), examples.end(),
              [](const ProjectInfo& a, const ProjectInfo& b) { return a.Name < b.Name; });
    return examples;
}

void ProjectLauncherPanel::TryOpen(const std::filesystem::path& path, std::optional<Result>& result)
{
    ProjectInfo info;
    std::string error;
    if (!Project::Load(path, info, &error))
    {
        SetMessage("Could not open '" + path.string() + "': " + error);
        return;
    }
    SetMessage("");
    result = Result{.Project = std::move(info)};
}

bool ProjectLauncherPanel::Matches(const Entry& entry) const
{
    if (m_Filter[0] == '\0')
        return true;
    const std::string filter = Lower(m_Filter);
    return Lower(entry.Name).find(filter) != std::string::npos ||
           Lower(entry.Root.string()).find(filter) != std::string::npos;
}

std::optional<ProjectLauncherPanel::Result> ProjectLauncherPanel::OnImGuiRender()
{
    std::optional<Result> result;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    ImGui::Begin("Project Manager", nullptr, flags);
    ImGui::PopStyleVar();

    DrawHeader();
    DrawMessage();

    const float gap = 28.0f;
    const float rightWidth = std::clamp(ImGui::GetContentRegionAvail().x * 0.36f, 300.0f, 460.0f);
    const float leftWidth = std::max(ImGui::GetContentRegionAvail().x - rightWidth - gap, 260.0f);

    ImGui::BeginChild("##Lists", ImVec2(leftWidth, 0.0f));
    DrawLists(result);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);
    ImGui::BeginChild("##Start", ImVec2(0.0f, 0.0f));
    DrawNewProject(result);
    ImGui::Spacing();
    ImGui::Spacing();
    DrawOpenExisting(result);
    ImGui::EndChild();

    ImGui::End();
    return result;
}

void ProjectLauncherPanel::DrawHeader()
{
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGuiIO& io = ImGui::GetIO();
    ImFont* boldFont = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
    if (boldFont)
        ImGui::PushFont(boldFont);
    ImGui::TextColored(EditorTheme::Accent(), "UNHUMAN ENGINE");
    if (boldFont)
        ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("  Project Manager");

    const float searchWidth = 260.0f;
    ImGui::SameLine(right - searchWidth);
    EditorUI::SearchField("##ProjectFilter", m_Filter, sizeof(m_Filter), "Filter projects", searchWidth);
    ImGui::Separator();
    ImGui::Spacing();
}

void ProjectLauncherPanel::DrawMessage()
{
    if (m_Message.empty())
        return;
    // Above the lists, not below them: the lists fill the window, and a
    // message under them was never on screen.
    const ImVec4 color = m_MessageIsError ? EditorTheme::Danger() : EditorTheme::Success();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(color.x, color.y, color.z, 0.14f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::BeginChild("##Message", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 30.0f);
    ImGui::TextUnformatted(m_Message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 22.0f);
    if (ImGui::SmallButton("x##DismissMessage"))
        m_Message.clear();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

bool ProjectLauncherPanel::DrawEntry(const Entry& entry, bool removable, std::optional<Result>& result)
{
    bool remove = false;
    // The list goes into the id too: an example the user opened is in both lists,
    // and two rows (and their context menus) with one id answer each other.
    ImGui::PushID(removable ? "Recent" : "Example");
    ImGui::PushID(entry.Root.string().c_str());

    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float rowHeight = lineHeight * 2.0f + style.ItemSpacing.y + 12.0f;
    const float removeWidth = removable ? 28.0f : 0.0f;
    const float width = ImGui::GetContentRegionAvail().x - removeWidth;
    const ImVec2 start = ImGui::GetCursorPos();

    // One click opens: the row is the button. A missing project cannot open,
    // but stays clickable for its menu and its remove button.
    if (ImGui::Selectable("##Row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(width, rowHeight)) && entry.Exists)
        TryOpen(entry.Root, result);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", entry.Exists ? entry.Root.string().c_str() : "This folder no longer holds a project");

    if (ImGui::BeginPopupContextItem("##RowMenu"))
    {
        if (ImGui::MenuItem("Open", nullptr, false, entry.Exists))
            TryOpen(entry.Root, result);
        if (ImGui::MenuItem("Show in File Manager", nullptr, false, entry.Exists))
            FileDialogs::OpenInDefaultApp(entry.Root.string());
        if (ImGui::MenuItem("Copy Path"))
            ImGui::SetClipboardText(entry.Root.string().c_str());
        if (removable)
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Remove from List"))
                remove = true;
        }
        ImGui::EndPopup();
    }

    // Name and status on the first line, the folder on the second.
    ImGui::SetCursorPos(ImVec2(start.x + 10.0f, start.y + 6.0f));
    ImGuiIO& io = ImGui::GetIO();
    ImFont* boldFont = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
    if (boldFont)
        ImGui::PushFont(boldFont);
    if (entry.Exists)
        ImGui::TextUnformatted(entry.Name.c_str());
    else
        ImGui::TextDisabled("%s", entry.Name.c_str());
    if (boldFont)
        ImGui::PopFont();
    ImGui::SameLine();
    if (!entry.Exists)
        EditorUI::Badge("missing", EditorTheme::Danger());
    else if (entry.StartScene.empty())
        EditorUI::Badge("no start scene", EditorTheme::Warning());
    else
        ImGui::TextDisabled("%s", std::filesystem::path(entry.StartScene).filename().string().c_str());

    ImGui::SetCursorPosX(start.x + 10.0f);
    DimText(entry.Root.string(), width - 20.0f);

    if (removable)
    {
        ImGui::SetCursorPos(ImVec2(start.x + width + 4.0f, start.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::SmallButton("x##Remove"))
            remove = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove from the list (the folder is not touched)");
    }
    ImGui::SetCursorPos(ImVec2(start.x, start.y + rowHeight + style.ItemSpacing.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    ImGui::PopID();
    ImGui::PopID();
    return remove;
}

void ProjectLauncherPanel::DrawLists(std::optional<Result>& result)
{
    EditorUI::SectionHeader("Recent");
    const bool anyMissing =
        std::any_of(m_RecentEntries.begin(), m_RecentEntries.end(), [](const Entry& e) { return !e.Exists; });
    if (anyMissing)
    {
        if (ImGui::SmallButton("Remove Missing"))
        {
            std::vector<std::filesystem::path> missing;
            for (const Entry& entry : m_RecentEntries)
                if (!entry.Exists)
                    missing.push_back(entry.Root);
            for (const std::filesystem::path& root : missing)
                std::erase(m_Recent, root);
            Save();
            Refresh();
        }
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Refresh"))
        Refresh();

    std::optional<std::filesystem::path> removed;
    std::size_t shown = 0;
    for (const Entry& entry : m_RecentEntries)
    {
        if (!Matches(entry))
            continue;
        ++shown;
        if (DrawEntry(entry, true, result))
            removed = entry.Root;
    }
    if (shown == 0)
        ImGui::TextDisabled(m_RecentEntries.empty() ? "No recent projects yet: create one, or open an example below."
                                                    : "No recent project matches the filter.");
    if (removed)
        RemoveRecent(*removed); // after the loop: it rebuilds the list being drawn

    if (!m_ExampleEntries.empty())
    {
        ImGui::Spacing();
        EditorUI::SectionHeader("Examples");
        for (const Entry& entry : m_ExampleEntries)
            if (Matches(entry))
                DrawEntry(entry, false, result);
    }
}

void ProjectLauncherPanel::DrawNewProject(std::optional<Result>& result)
{
    EditorUI::SectionHeader("New Project");

    ImGui::TextDisabled("Name");
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputText("##ProjectName", m_NewProjectName, sizeof(m_NewProjectName),
                                        ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::Spacing();
    ImGui::TextDisabled("Location");
    const float browseWidth = 80.0f;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    DimText(m_NewProjectParent.string(), right - ImGui::GetCursorPosX() - browseWidth - 8.0f);
    ImGui::SameLine(right - browseWidth);
    if (ImGui::Button("Browse...", ImVec2(browseWidth, 0.0f)))
    {
        const std::string folder = FileDialogs::SelectFolder("Create the project in", m_NewProjectParent.string());
        if (!folder.empty())
        {
            m_NewProjectParent = Project::NormalizeRoot(folder);
            Save();
        }
    }

    ImGui::Spacing();
    ImGui::Checkbox("Start with a scene", &m_NewProjectStarter);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Ground, a cube, a sun and a camera in Assets/Scenes/Main.unhuman,\n"
                          "set as the start scene. Off: an empty project.");

    // Checked as it is typed, so Create is never a guess.
    const std::string problem = Project::CheckNewProject(m_NewProjectParent, m_NewProjectName);
    ImGui::Spacing();
    if (problem.empty())
    {
        const std::string target = (m_NewProjectParent / m_NewProjectName).string();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Creates %s", target.c_str());
        ImGui::PopTextWrapPos();
    }
    else
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(EditorTheme::Warning(), "%s", problem.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!problem.empty());
    ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Accent());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::AccentHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::AccentActive());
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::AccentForeground());
    const bool create = ImGui::Button("Create", ImVec2(-1.0f, 34.0f));
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();

    if ((create || enter) && problem.empty())
    {
        ProjectInfo created;
        std::string error;
        if (Project::Create(m_NewProjectParent / m_NewProjectName, m_NewProjectName, created, &error))
        {
            SetMessage("");
            Save();
            result = Result{.Project = std::move(created), .Created = true, .StarterScene = m_NewProjectStarter};
        }
        else
        {
            SetMessage("Could not create the project: " + error);
        }
    }
}

void ProjectLauncherPanel::DrawOpenExisting(std::optional<Result>& result)
{
    EditorUI::SectionHeader("Open");
    if (ImGui::Button("Open Project Folder...", ImVec2(-1.0f, 30.0f)))
    {
        const std::string folder = FileDialogs::SelectFolder("Open Project", m_NewProjectParent.string());
        if (!folder.empty())
            TryOpen(folder, result);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("A folder holding a %s", Project::kManifestName);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("A project is a folder with a %s and an Assets folder. Its Assets are mounted as game:, "
                        "so scenes refer to files by paths relative to it.",
                        Project::kManifestName);
    ImGui::PopTextWrapPos();
}
} // namespace UHE
