#pragma once

// Colours shared by the scene hierarchy (outliner) and its component
// inspector. Internal to these two translation units.
#include <imgui/imgui.h>
#include "EditorTheme.h"

namespace UHE
{

// Theme-driven accents (previously hardcoded violet constants). Fully
// qualified: EditorTheme is a global-namespace library, not part of UHE.
inline ImVec4 kAccent()
{
    return ::EditorTheme::Accent();
}
inline ImVec4 kAccentHover()
{
    return ::EditorTheme::AccentHover();
}
inline ImVec4 kAccentActive()
{
    return ::EditorTheme::AccentActive();
}
inline ImVec4 kAccentMuted()
{
    return ::EditorTheme::AccentMuted();
}
// Readable label color for accent-filled controls (fixes white-on-white on
// light-accent themes like Carbon).
inline ImVec4 kAccentText()
{
    return ::EditorTheme::AccentForeground();
}

// Dimmed label text derived from the active theme's text color (the old
// near-white constants were unreadable on the light theme).
inline ImVec4 DimText(float alpha)
{
    ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    return ImVec4(c.x, c.y, c.z, alpha);
}
inline constexpr ImVec4 kAxisX{0.75f, 0.22f, 0.28f, 1.0f};
inline constexpr ImVec4 kAxisXHover{0.85f, 0.32f, 0.35f, 1.0f};
inline constexpr ImVec4 kAxisXActive{0.65f, 0.15f, 0.20f, 1.0f};
inline constexpr ImVec4 kAxisY{0.30f, 0.68f, 0.35f, 1.0f};
inline constexpr ImVec4 kAxisYHover{0.38f, 0.78f, 0.42f, 1.0f};
inline constexpr ImVec4 kAxisYActive{0.22f, 0.58f, 0.28f, 1.0f};
inline constexpr ImVec4 kAxisZ{0.25f, 0.42f, 0.82f, 1.0f};
inline constexpr ImVec4 kAxisZHover{0.35f, 0.52f, 0.92f, 1.0f};
inline constexpr ImVec4 kAxisZActive{0.18f, 0.32f, 0.72f, 1.0f};

/// @brief Share of the inspector width given to the property label column.
/// Every section uses the same ratio so labels and values line up down the
/// whole panel, as in Godot's Inspector and Blender's Properties editor.
inline constexpr float kPropertyLabelRatio = 0.40f;

/// @brief Colour of a property's name in the label column.
/// Dimmer than the value so the eye reads values first; derived from the
/// theme's text colour so it stays legible on the light themes.
inline ImVec4 PropertyLabelText()
{
    return DimText(0.72f);
}

/// @brief Whether the active theme has a light window background.
/// Decided from the style rather than the theme id, so a custom or newly
/// added theme is classified correctly without a list to keep in step.
inline bool IsLightBackground()
{
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    return bg.x * 0.2126f + bg.y * 0.7152f + bg.z * 0.0722f > 0.5f;
}

/// @brief Colour that marks an axis (0 = X, 1 = Y, 2 = Z) on a vector field.
/// The darker kAxis* values keep contrast against a light frame; a dark frame
/// needs brighter ones, because the tag is a thin stripe and a letter rather
/// than a filled button. Authored in sRGB, as a colour picker shows them.
inline ImVec4 AxisColor(int axis)
{
    static constexpr ImVec4 kOnDark[3] = {
        {0.93f, 0.36f, 0.40f, 1.0f},
        {0.53f, 0.80f, 0.30f, 1.0f},
        {0.38f, 0.60f, 0.98f, 1.0f},
    };
    static constexpr ImVec4 kOnLight[3] = {kAxisX, kAxisY, kAxisZ};
    const int index = axis < 0 ? 0 : (axis > 2 ? 2 : axis);
    return IsLightBackground() ? kOnLight[index] : kOnDark[index];
}

} // namespace UHE
