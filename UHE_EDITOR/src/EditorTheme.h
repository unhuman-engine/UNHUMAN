#pragma once

#include "imgui.h"

namespace EditorTheme
{
    enum class EditorThemeId
    {
        DarkViolet = 0,
        Midnight,
        Graphite,
        Light,
        Emerald,
        Rose,
        Ocean,
        Sunset,
        Nord,
        Dracula,
        Paper,
        Carbon,
        COUNT
    };

    // Applies the theme to the ImGui style (colors + metrics).
    void Apply(EditorThemeId id);

    // Applies the persisted selection once, on the first ImGui frame.
    void EnsureApplied();

    EditorThemeId GetSelected();
    const char* GetName(EditorThemeId id);
    int GetThemeCount();

    // Applies and persists the selection.
    void SetSelected(EditorThemeId id);

    // Reads the persisted theme + custom accent (defaults when missing).
    void LoadSelected();

    // ---- custom accent picker ----
    // When enabled, the accent color overrides the theme's built-in accent
    // everywhere (checkmarks, sliders, headers, buttons, drag-drop targets,
    // panel accents...). Backgrounds/text of the chosen theme are kept.
    bool IsCustomAccentEnabled();
    ImVec4 GetCustomAccent();
    void SetCustomAccentEnabled(bool enabled); // re-applies + persists
    void SetCustomAccent(const ImVec4& rgb);   // enables + re-applies + persists

    // ---- console / text palette (theme-aware) ----
    ImVec4 ConsoleInfo();
    ImVec4 ConsoleWarn();
    ImVec4 ConsoleError();
    ImVec4 ConsoleCritical();

    // Palette accessors for panels with hand-drawn accents.
    ImVec4 Accent();
    ImVec4 AccentHover();
    ImVec4 AccentActive();
    ImVec4 AccentMuted();
    ImVec4 ToolbarBg();
    // Readable text color to use ON TOP of Accent()-filled controls (near-white
    // accents like Carbon's get dark text; dark accents get white).
    ImVec4 AccentForeground();
} // namespace EditorTheme
