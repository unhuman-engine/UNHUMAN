#include "CommandPalette.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include "EditorTheme.h"
#include "imgui.h"

namespace UHE
{
namespace
{
char ToLower(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Subsequence match over a lowercase query. Consecutive and
// word-start hits score higher; -1 means "does not match".
int FuzzyScore(const char* haystack, const char* needle)
{
    if (needle[0] == '\0')
        return 0;

    int score = 0;
    int streak = 0;
    int position = 0;

    for (int i = 0; haystack[i] != '\0'; ++i)
    {
        if (ToLower(haystack[i]) != ToLower(needle[position]))
        {
            streak = 0;
            continue;
        }

        score += 10 + streak * 6;
        if (i == 0 || haystack[i - 1] == ' ' || haystack[i - 1] == '/' || haystack[i - 1] == '(')
            score += 18;

        streak++;
        if (needle[++position] == '\0')
            return score - i; // earlier matches win ties
    }

    return -1;
}
} // namespace

void CommandPalette::Open()
{
    m_Open = true;
    m_FocusInput = true;
    m_ResetScroll = true;
    m_Query[0] = '\0';
    m_Selected = 0;
    m_Matches.clear();
    m_PendingAction = nullptr;
}

void CommandPalette::Close()
{
    m_Open = false;
    m_FocusInput = false;
    m_PendingAction = nullptr;
}

void CommandPalette::OnImGuiRender(const std::vector<Command>& commands)
{
    if (!m_Open)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 workspaceMin = viewport->Pos;
    const ImVec2 workspaceMax(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y);
    ImGui::GetBackgroundDrawList()->AddRectFilled(workspaceMin, workspaceMax, IM_COL32(0, 0, 0, 120));

    const ImVec2 size(640.0f, 430.0f);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.28f), ImGuiCond_Always,
        ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, EditorTheme::ToolbarBg());
    ImGui::Begin("##CommandPalette", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::ConsoleInfo());
    ImGui::TextUnformatted("COMMAND PALETTE");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("  type to filter, Enter to run, Esc to close");

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    if (m_FocusInput)
    {
        ImGui::SetKeyboardFocusHere();
        m_FocusInput = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool queryChanged =
        ImGui::InputTextWithHint("##CommandQuery", "Search commands, panels, themes...", m_Query, sizeof(m_Query));
    ImGui::PopStyleVar();
    if (queryChanged)
        m_Selected = 0;

    // Rebuild the ranking every frame: the command list itself is rebuilt
    // by the editor, so scores must not be cached across frames.
    m_Matches.clear();
    for (int i = 0; i < static_cast<int>(commands.size()); ++i)
    {
        const Command& command = commands[static_cast<std::size_t>(i)];
        const std::string searchable = command.Label + " " + command.Category;
        const int score = FuzzyScore(searchable.c_str(), m_Query);
        if (score >= 0)
            m_Matches.push_back({i, score});
    }

    std::stable_sort(m_Matches.begin(), m_Matches.end(),
                     [](const Match& a, const Match& b) { return a.Score > b.Score; });

    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        m_Selected++;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        m_Selected--;
    if (!m_Matches.empty())
        m_Selected = std::clamp(m_Selected, 0, static_cast<int>(m_Matches.size()) - 1);
    else
        m_Selected = 0;

    const bool runSelected = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        Close();

    std::function<void()> clickedAction;

    const float footerHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    ImGui::BeginChild("##CommandResults", ImVec2(0.0f, -footerHeight), false);
    if (m_ResetScroll)
    {
        ImGui::SetScrollY(0.0f);
        m_ResetScroll = false;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImFont* boldFont = ImGui::GetIO().Fonts->Fonts.Size > 1 ? ImGui::GetIO().Fonts->Fonts[1] : nullptr;
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 shortcutColor = ImGui::GetColorU32(EditorTheme::AccentMuted());

    for (int row = 0; row < static_cast<int>(m_Matches.size()); ++row)
    {
        const Command& command = commands[static_cast<std::size_t>(m_Matches[row].Index)];
        const bool selected = row == m_Selected;
        const bool enabled = command.Enabled;

        ImGui::PushID(row);
        if (!enabled)
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

        char id[32];
        std::snprintf(id, sizeof(id), "##cmd%d", row);
        const bool clicked =
            ImGui::Selectable(id, selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, 30.0f));
        const bool doubleClicked = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

        if (!enabled)
            ImGui::PopStyleColor();

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            m_Selected = row;

        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const float textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5f;

        // Measure the label with the font it is actually drawn in: the bold
        // face is wider, and measuring with the default one made the
        // category collide with the label ("New SceneFile").
        float labelWidth = 0.0f;
        if (boldFont)
        {
            ImGui::PushFont(boldFont);
            labelWidth = ImGui::CalcTextSize(command.Label.c_str()).x;
            ImGui::PopFont();
            draw->AddText(boldFont, boldFont->LegacySize, ImVec2(rowMin.x + 10.0f, textY),
                          enabled ? textColor : dimColor, command.Label.c_str());
        }
        else
        {
            labelWidth = ImGui::CalcTextSize(command.Label.c_str()).x;
            draw->AddText(ImVec2(rowMin.x + 10.0f, textY), enabled ? textColor : dimColor, command.Label.c_str());
        }

        if (!command.Category.empty())
        {
            draw->AddText(ImVec2(rowMin.x + 10.0f + labelWidth + 12.0f, textY),
                          selected ? ImGui::GetColorU32(ImGuiCol_Text, 0.70f) : dimColor, command.Category.c_str());
        }

        if (!command.Shortcut.empty())
        {
            const float shortcutWidth = ImGui::CalcTextSize(command.Shortcut.c_str()).x;
            draw->AddText(ImVec2(rowMax.x - shortcutWidth - 10.0f, textY),
                          enabled ? (selected ? textColor : shortcutColor) : dimColor, command.Shortcut.c_str());
        }

        if (selected)
            ImGui::SetScrollHereY(0.5f);

        if (clicked && enabled && command.Action)
            clickedAction = command.Action;

        ImGui::PopID();
    }

    if (clickedAction)
        m_PendingAction = clickedAction;
    else if (runSelected && !m_Matches.empty())
    {
        const Command& selectedCommand = commands[static_cast<std::size_t>(m_Matches[m_Selected].Index)];
        if (selectedCommand.Enabled && selectedCommand.Action)
            m_PendingAction = selectedCommand.Action;
    }

    if (m_Matches.empty())
        ImGui::TextDisabled("No command matches \"%s\".", m_Query);

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextDisabled("%d command%s available", static_cast<int>(m_Matches.size()), m_Matches.size() == 1 ? "" : "s");
    ImGui::SameLine(ImGui::GetWindowWidth() - 250.0f);
    ImGui::TextDisabled("Enter run   Up/Down select   Esc close");

    ImGui::End();

    if (m_PendingAction)
    {
        // Run outside the palette's draw scope: commands toggle windows,
        // reset the layout and open modals, all of which re-enter ImGui.
        std::function<void()> action = std::move(m_PendingAction);
        m_PendingAction = nullptr;
        Close();
        action();
    }
}
} // namespace UHE
