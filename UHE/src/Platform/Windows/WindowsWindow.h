#pragma once

#include <GLFW/glfw3.h>
#include "UHE/Core/Window.h"

namespace UHE
{
class WindowsWindow : public Window
{
public:
    WindowsWindow(const WindowProps& props);
    ~WindowsWindow();

    void OnUpdate() override;

    inline unsigned int GetWidth() const override { return m_Data.Width; }
    inline unsigned int GetHeight() const override { return m_Data.Height; }

    // Window attributes
    inline void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
    void SetVSync(bool enabled) override;
    bool IsVSync() const override;
    void SetCursorLocked(bool locked) override;

    void SetWindowMode(WindowMode mode, i32 monitor, u32 width, u32 height, u32 refreshRate) override;
    [[nodiscard]] WindowMode GetWindowMode() const override { return m_Mode; }
    void SetWindowedSize(u32 width, u32 height) override;
    void MoveToMonitor(i32 monitor) override;
    [[nodiscard]] std::vector<MonitorInfo> GetMonitors() const override;
    [[nodiscard]] i32 GetCurrentMonitor() const override;
    [[nodiscard]] std::vector<VideoMode> GetVideoModes(i32 monitor) const override;

    inline void* GetNativeWindow() const override { return m_Window; }

private:
    virtual void Init(const WindowProps& props);
    virtual void Shutdown();
    /// @brief Records the windowed placement that leaving Windowed must restore.
    void SaveWindowedRect();

private:
    GLFWwindow* m_Window;
    WindowMode m_Mode = WindowMode::Windowed;
    /// @brief Windowed placement, kept while borderless or fullscreen.
    struct WindowedRect
    {
        int X = 0, Y = 0;
        int Width = 0, Height = 0;
        bool Maximized = false;
    };
    WindowedRect m_Windowed;
    /// @brief SetVSync ran since the renderer last heard of it. The window can be
    /// told before the renderer exists, so OnUpdate forwards it.
    bool m_VSyncPending = true;
    struct WindowData
    {
        std::string Title;
        unsigned int Width, Height;
        bool VSync;
        EventCallbackFn EventCallback;
    };
    WindowData m_Data;
};

} // namespace UHE
