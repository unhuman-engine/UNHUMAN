#pragma once

#include <functional>
#include <string>
#include <vector>

namespace UHE
{
// Spotlight-style command launcher (Ctrl+P). The editor rebuilds the
// command list every frame, so entries always reflect the current scene,
// project and panel state; the palette only owns the query and selection.
class CommandPalette
{
public:
    struct Command
    {
        std::string Label;
        std::string Category; // "File", "View", "Play", ...
        std::string Shortcut; // "Ctrl+S" or empty
        bool Enabled = true;
        std::function<void()> Action;
    };

    void Open();
    void Close();
    void Toggle() { m_Open ? Close() : Open(); }
    [[nodiscard]] bool IsOpen() const { return m_Open; }

    // Draws the palette when open. Must be called after the dockspace and
    // the panels, so it renders on top of everything else.
    void OnImGuiRender(const std::vector<Command>& commands);

private:
    struct Match
    {
        int Index = 0;
        int Score = 0;
    };

    bool m_Open = false;
    bool m_FocusInput = false;
    bool m_ResetScroll = false;
    char m_Query[128] = {};
    int m_Selected = 0;
    std::vector<Match> m_Matches;
    std::function<void()> m_PendingAction;
};
} // namespace UHE
