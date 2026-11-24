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

} // namespace UHE
