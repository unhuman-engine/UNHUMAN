#pragma once
#include "Core.h"
#include "UHE/Core/CaptureSettings.h"
#include "UHE/Core/LayerStack.h"
#include "UHE/Core/Timestep.h"
#include "UHE/Events/ApplicationEvent.h"
#include "UHE/ImGui/ImGuiLayer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/RHISelfTest.h"
#include "Window.h"

namespace UHE
{

class UHE_API Application
{
public:
    Application(const std::string& name = "UHE APP");
    /// @brief Opens the main window with \p window instead of the default 1920x1080.
    Application(const std::string& name, const WindowProps& window);
    virtual ~Application();

    void Run();

    void OnEvent(Event& e);

    void PushLayer(Layer* layer);
    void PushOverlay(Layer* layer);

    ImGuiLayer* GetImguiLayer() { return m_ImGuiLayer; };

    void Close();
    /// @brief Process exit code: non-zero when a requested frame capture failed.
    [[nodiscard]] int GetExitCode() const { return m_ExitCode; }
    /// @brief The capture/fixed-timestep test hook read at startup.
    [[nodiscard]] const CaptureSettings& GetCaptureSettings() const { return m_Capture; }
    // bool OnWindowResize(WindowResizeEvent& e);

    inline Window& GetWindow() { return *m_Window; }
    inline static Application& Get() { return *s_instance; }

private:
    bool OnWindowClose(WindowCloseEvent& e);
    bool OnWindowResize(WindowResizeEvent& e);

    std::unique_ptr<Window> m_Window;
    ImGuiLayer* m_ImGuiLayer;
    bool m_Running = true;
    bool m_Minimized = false;

    LayerStack m_LayerStack;
    double m_LastFrameTime = 0.0; ///< @brief glfwGetTime() at the last frame.
    /// @brief Asks the device for this frame's pixels when it is the one to capture.
    void RequestCaptureIfDue();
    /// @brief Writes a captured frame as PNG and ends the run.
    void OnFrameCaptured(const RHI::RHIDevice::CapturedFrame& frame);
    CaptureSettings m_Capture;
    u32 m_RenderedFrames = 0; ///< @brief frames that recorded GPU work (skipped ones excluded)
    int m_ExitCode = 0;
    // UHE_RHI_SELFTEST (issue #30, #32): the device checks, then the frames part.
    std::unique_ptr<class RHISelfTestFrames> m_SelfTestFrames;
    RHISelfTestResult m_SelfTestResult;
    bool m_SelfTestExit = false;

private:
    static Application* s_instance;
};

// T0 be define in class
Application* CreateApplication();
} // namespace UHE
