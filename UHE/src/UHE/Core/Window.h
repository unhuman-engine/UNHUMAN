#pragma once

#include <uhepch.h>
#include "UHE/Core/Core.h"
#include "UHE/Events/Event.h"

namespace UHE
{
struct UHE_API WindowProps
{
    std::string Title;
    u32 Width;
    u32 Height;

    WindowProps(const std::string& title = "UHE ENGINE", u32 width = 1920, u32 height = 1080)
        : Title(title), Width(width), Height(height)
    {
    }
};

/// @brief How the window occupies its monitor.
enum class WindowMode : u8
{
    Windowed = 0, ///< decorated window, freely sized and placed
    Borderless,   ///< undecorated window covering the monitor at its desktop resolution
    Fullscreen,   ///< exclusive fullscreen; may switch the monitor's video mode
};

/// @brief A monitor as the window system reports it: its position on the virtual desktop and current mode.
struct UHE_API MonitorInfo
{
    std::string Name;
    i32 X = 0;
    i32 Y = 0;
    u32 Width = 0;
    u32 Height = 0;
    u32 RefreshRate = 0;
};

/// @brief A video mode a monitor can switch to in exclusive fullscreen.
struct UHE_API VideoMode
{
    u32 Width = 0;
    u32 Height = 0;
    u32 RefreshRate = 0;
};

// interface represented window over the os desktop system
class UHE_API Window
{
public:
    using EventCallbackFn = std::function<void(Event&)>;

    virtual ~Window() = default;

    virtual void OnUpdate() = 0;

    virtual u32 GetWidth() const = 0;
    virtual u32 GetHeight() const = 0;

    // Window attributes
    virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
    /// @brief The text in the window's title bar.
    virtual void SetTitle(const std::string& title) = 0;
    /// @brief Asks for vertical sync. Reaches the renderer's swapchain on the next OnUpdate().
    virtual void SetVSync(bool enabled) = 0;
    virtual bool IsVSync() const = 0;

    // Lock/hide cursor for FPS-style mouse look
    virtual void SetCursorLocked(bool locked) = 0;

    // Display mode
    /**
     * @brief Switches between windowed, borderless and exclusive fullscreen.
     *
     * Leaving Windowed remembers the window's position and size (and whether it
     * was maximized); returning to Windowed restores them.
     *
     * @param mode         The mode to switch to.
     * @param monitor      Index into GetMonitors(); -1 is the monitor the window is on.
     * @param width,height Fullscreen resolution; 0 keeps the monitor's current one. Ignored otherwise.
     * @param refreshRate  Fullscreen refresh rate in Hz; 0 keeps the current one. Ignored otherwise.
     */
    virtual void SetWindowMode(WindowMode mode, i32 monitor = -1, u32 width = 0, u32 height = 0,
                               u32 refreshRate = 0) = 0;
    [[nodiscard]] virtual WindowMode GetWindowMode() const = 0;
    /// @brief Resizes a windowed window; in the other modes, sets the size Windowed returns to.
    virtual void SetWindowedSize(u32 width, u32 height) = 0;
    /// @brief Moves a windowed window to the centre of \p monitor (index into GetMonitors()).
    virtual void MoveToMonitor(i32 monitor) = 0;
    /// @brief The connected monitors; index 0 is the primary one.
    [[nodiscard]] virtual std::vector<MonitorInfo> GetMonitors() const = 0;
    /// @brief The monitor the window is fullscreen on, or the one holding most of it; -1 without monitors.
    [[nodiscard]] virtual i32 GetCurrentMonitor() const = 0;
    /// @brief The video modes \p monitor supports, smallest first.
    [[nodiscard]] virtual std::vector<VideoMode> GetVideoModes(i32 monitor) const = 0;

    virtual void* GetNativeWindow() const = 0;

    static Window* Create(const WindowProps& props = WindowProps());
};

} // namespace UHE
