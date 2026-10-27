#pragma once

#include "imgui.h"
#include <cstddef>

namespace EditorUI
{
    enum class ToolIcon
    {
        Select,
        Move,
        Rotate,
        Scale,
    };

    enum class TransportIcon
    {
        Play,
        Pause,
        Stop,
    };

    // Compact square tool button with a hand-drawn icon (no icon font needed).
    bool ToolIconButton(const char* id, ToolIcon icon, bool active, const char* tooltip, float size = 30.0f);

    // Playback transport button drawn with the theme color (avoids dark bitmap icons).
    bool TransportButton(const char* id, TransportIcon icon, bool enabled, bool active,
                         const char* tooltip, const ImVec4& color, float size = 28.0f);

    // Text button that highlights with the theme accent while active.
    bool ToolbarToggle(const char* label, bool active, const char* tooltip = nullptr, const ImVec2& size = ImVec2(0, 0));

    // Uppercased section rule used inside inspector/settings panels.
    void SectionHeader(const char* label);

    // Small rounded label, e.g. "Playing" or "Edit".
    void Badge(const char* label, const ImVec4& color);

    // Search input with a rounded frame; returns true while edited.
    bool SearchField(const char* id, char* buffer, std::size_t bufferSize, const char* hint, float width = 0.0f);

    // View/Window menu entry with a checkmark and optional shortcut.
    bool ToggleMenuItem(const char* label, bool* value, const char* shortcut = nullptr);

    // Thin horizontal rule; usually followed by SameLine() by the caller.
    void Separator();
} // namespace EditorUI
