#pragma once

#include <string>
#include "UHE/Core/Core.h"
#include "UHE/Core/Window.h"

namespace UHE
{
/**
 * @brief The editor's display preferences: window mode, window size, monitor,
 *        VSync and the scene viewport's render scale.
 *
 * Persisted in editor_display.ini next to editor_theme.ini and applied at
 * startup. Changes made in the UI reach the window in ApplyPending(), which
 * the editor calls from OnUpdate: a mode switch resizes the framebuffer, which
 * should not happen in the middle of the ImGui frame that asked for it.
 */
class EditorDisplaySettings
{
public:
    static constexpr f32 kMinRenderScale = 0.5f;
    static constexpr f32 kMaxRenderScale = 2.0f;

    /// @brief Reads editor_display.ini. Anything missing or invalid keeps its default.
    void Load();
    /// @brief Writes editor_display.ini; a read-only directory is ignored, as for the theme.
    void Save() const;

    /// @brief Applies every loaded setting to \p window, once at startup.
    void ApplyAll(Window& window);
    /// @brief Applies what the Display section changed since the last call.
    void ApplyPending(Window& window);
    /// @brief Keeps the windowed size the window has now, so the next session opens at it.
    void CaptureWindowSize(const Window& window);

    /// @brief Draws the Display section's rows into the preferences.
    void Draw(Window& window);

    [[nodiscard]] f32 GetRenderScale() const { return m_RenderScale; }
    /**
     * @brief The pixel size a viewport \p panelSize wide renders at.
     * @return \p panelSize times the render scale, rounded, within [1, \p maxSize].
     */
    [[nodiscard]] u32 ScaledSize(f32 panelSize, u32 maxSize) const;

private:
    /// @brief Index of the chosen monitor in Window::GetMonitors(), or -1 for the window's current one.
    [[nodiscard]] i32 ResolveMonitor(const Window& window) const;

    WindowMode m_Mode = WindowMode::Windowed;
    std::string m_MonitorName; ///< empty: the monitor the window opens on
    i32 m_MonitorIndex = -1;   ///< tells two monitors with the same name apart
    u32 m_WindowedWidth = 0;   ///< 0: the window's default size
    u32 m_WindowedHeight = 0;
    u32 m_FullscreenWidth = 0; ///< 0: the monitor's current video mode
    u32 m_FullscreenHeight = 0;
    u32 m_FullscreenRefresh = 0;
    bool m_VSync = false; ///< off, as the engine starts without it
    f32 m_RenderScale = 1.0f;

    bool m_ModeDirty = false;
    bool m_MonitorDirty = false;
    bool m_SizeDirty = false;
};
} // namespace UHE
