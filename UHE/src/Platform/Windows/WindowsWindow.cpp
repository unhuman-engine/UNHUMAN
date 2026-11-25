#include "uhepch.h"
#include "WindowsWindow.h"
#include <stb_image.h>
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Core/Log.h"
#include "UHE/Events/ApplicationEvent.h"
#include "UHE/Events/KeyEvent.h"
#include "UHE/Events/MouseEvent.h"
#include "UHE/Renderer/Renderer.h"

namespace UHE {
static bool s_GLFWInitialized = false;

static void GLFWErrorCallback(int error, const char *descrioption) {
  UHE_CORE_ERROR("GLFW_ERROR({0}):{1}", error, descrioption);
}

Window *Window::Create(const WindowProps &props) {
  UHE_PROFILE_FUNCTION();
  return new WindowsWindow(props);
}
WindowsWindow::WindowsWindow(const WindowProps &props) {
  UHE_PROFILE_FUNCTION();
  Init(props);
}
WindowsWindow::~WindowsWindow() { Shutdown(); }

void WindowsWindow::Init(const WindowProps &props) {
  m_Data.Title = props.Title;
  m_Data.Width = props.Width;
  m_Data.Height = props.Height;
  m_Windowed.Width = static_cast<int>(props.Width);
  m_Windowed.Height = static_cast<int>(props.Height);

  UHE_CORE_INFO("CREATE WINDOW {0} : {1}, {2}", props.Title, props.Width,
               props.Height);

  if (!s_GLFWInitialized) {
    int success = glfwInit();
    UHE_CORE_ASSERT(success, "Could not initialize GLFW!");
    glfwSetErrorCallback(GLFWErrorCallback);
    s_GLFWInitialized = true;
  }

  // Tell GLFW we're using Vulkan, not OpenGL
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  m_Window = glfwCreateWindow((int)props.Width, (int)props.Height,
                              m_Data.Title.c_str(), nullptr, nullptr);
  GLFWimage images[1];
  const std::string path = UHE::FileSystem::Get().Resolve("icon/ace.jpg");

  images[0].pixels =
      stbi_load(path.c_str(), &images[0].width, &images[0].height, 0, 4);
  if (images[0].pixels) {
    glfwSetWindowIcon(m_Window, 1, images);
    stbi_image_free(images[0].pixels);
  } else {
    images[0].pixels =
        stbi_load(path.c_str(), &images[0].width, &images[0].height, 0, 4);
    if (images[0].pixels) {
      glfwSetWindowIcon(m_Window, 1, images);
      stbi_image_free(images[0].pixels);
    } else {
      UHE_CORE_WARN("Could not load window icon.");
    }
  }

  glfwSetWindowUserPointer(m_Window, &m_Data);
  SetVSync(false);

  // If ImGui backend installed callbacks earlier with install_callbacks=true,
  // it saved previous callbacks and chained them. When using
  // install_callbacks=false we must forward the callbacks manually; ensure
  // backend data is initialized by setting current context.

  // set GLFW callbacks here
  glfwSetWindowSizeCallback(
      m_Window, [](GLFWwindow *window, int width, int height) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);

        WindowResizeEvent event(width, height);
        data.Width = width;
        data.Height = height;
        data.EventCallback(event);
      });
  glfwSetWindowCloseCallback(m_Window, [](GLFWwindow *window) {
    WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);
    WindowCloseEvent event;
    data.EventCallback(event);
  });
  // Files dropped from the OS file explorer. GLFW reports the paths; the cursor
  // position at drop time tells the editor which panel received them.
  glfwSetDropCallback(m_Window,
                      [](GLFWwindow* window, int count, const char** paths)
                      {
                          WindowData& data = *(WindowData*)glfwGetWindowUserPointer(window);
                          if (count <= 0 || paths == nullptr)
                              return;

                          std::vector<std::string> files;
                          files.reserve(static_cast<std::size_t>(count));
                          for (int i = 0; i < count; ++i)
                          {
                              if (paths[i] != nullptr)
                                  files.emplace_back(paths[i]);
                          }

                          double cursorX = 0.0, cursorY = 0.0;
                          glfwGetCursorPos(window, &cursorX, &cursorY);

                          WindowFileDropEvent event(std::move(files), static_cast<float>(cursorX),
                                                    static_cast<float>(cursorY));
                          data.EventCallback(event);
                      });
  glfwSetKeyCallback(m_Window, [](GLFWwindow *window, int key, int scancode,
                                  int action, int mods) {
    WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);
    switch (action) {
    case GLFW_PRESS: {
      KeyPressedEvent event(key, 0);
      data.EventCallback(event);
      break;
    }
    case GLFW_RELEASE: {
      KeyReleasedEvent event(key);
      data.EventCallback(event);
      break;
    }
    case GLFW_REPEAT: {
      KeyPressedEvent event(key, 1);
      data.EventCallback(event);
      break;
    }
    }
  });
  glfwSetCharCallback(m_Window, [](GLFWwindow *window, unsigned int keycode) {
    WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);

    KeyTypedEvent event(keycode);
    data.EventCallback(event);
  });

  glfwSetMouseButtonCallback(
      m_Window, [](GLFWwindow *window, int button, int action, int mods) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);

        switch (action) {
        case GLFW_PRESS: {
          MouseButtonPressedEvent event(button);
          data.EventCallback(event);
          break;
        }
        case GLFW_RELEASE: {
          MouseButtonReleasedEvent event(button);
          data.EventCallback(event);
          break;
        }
        }
      });

  glfwSetScrollCallback(
      m_Window, [](GLFWwindow *window, double xOffset, double yOffset) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);

        MouseScrolledEvent event((float)xOffset, (float)yOffset);
        data.EventCallback(event);
      });

  glfwSetCursorPosCallback(
      m_Window, [](GLFWwindow *window, double xPos, double yPos) {
        WindowData &data = *(WindowData *)glfwGetWindowUserPointer(window);

        MouseMovedEvent event((float)xPos, (float)yPos);
        data.EventCallback(event);
      });
}

void WindowsWindow::Shutdown() {
  UHE_PROFILE_FUNCTION();
  glfwDestroyWindow(m_Window);
  /*--s_GLFWindowCount;
  if(s_GLFWindowCount == 0) {
          glfwTerminate();
  }*/
}

void WindowsWindow::OnUpdate() {
  glfwPollEvents();
  // Forwarded here rather than in SetVSync: the application sets it before the
  // renderer exists, and this only runs from the frame loop, after it does.
  if (m_VSyncPending)
  {
      Renderer::GetDevice().SetVSync(m_Data.VSync);
      m_VSyncPending = false;
  }
}

void WindowsWindow::SetTitle(const std::string& title)
{
    m_Data.Title = title;
    glfwSetWindowTitle(m_Window, m_Data.Title.c_str());
}

void WindowsWindow::SetVSync(bool enabled) {
  m_Data.VSync = enabled;
  m_VSyncPending = true;
}

bool WindowsWindow::IsVSync() const { return m_Data.VSync; }

void WindowsWindow::SetCursorLocked(bool locked) {
  glfwSetInputMode(m_Window, GLFW_CURSOR,
                   locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

namespace
{
/// @brief Wayland neither reports nor sets window positions, and GLFW logs an error for every try.
bool CanPlaceWindows()
{
    return glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
}

/// @brief The monitor at \p index in glfwGetMonitors(), or null when there is none.
GLFWmonitor* MonitorAt(i32 index)
{
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (monitors == nullptr || index < 0 || index >= count)
        return nullptr;
    return monitors[index];
}
} // namespace

void WindowsWindow::SaveWindowedRect()
{
    m_Windowed.Maximized = glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED) == GLFW_TRUE;
    glfwGetWindowSize(m_Window, &m_Windowed.Width, &m_Windowed.Height);
    if (CanPlaceWindows())
        glfwGetWindowPos(m_Window, &m_Windowed.X, &m_Windowed.Y);
}

void WindowsWindow::SetWindowMode(WindowMode mode, i32 monitor, u32 width, u32 height, u32 refreshRate)
{
    const i32 index = monitor >= 0 ? monitor : GetCurrentMonitor();
    GLFWmonitor* target = MonitorAt(index);
    if (mode != WindowMode::Windowed && target == nullptr)
    {
        UHE_CORE_WARN("SetWindowMode: there is no monitor {0}", index);
        return;
    }
    if (m_Mode == WindowMode::Windowed && mode == WindowMode::Windowed)
        return;
    if (m_Mode == WindowMode::Windowed)
        SaveWindowedRect();

    const int restoreWidth = std::max(m_Windowed.Width, 1);
    const int restoreHeight = std::max(m_Windowed.Height, 1);
    // Leave a monitor first: that restores its desktop video mode, which the
    // borderless size and "keep the current resolution" below have to read.
    if (glfwGetWindowMonitor(m_Window) != nullptr)
        glfwSetWindowMonitor(m_Window, nullptr, m_Windowed.X, m_Windowed.Y, restoreWidth, restoreHeight,
                             GLFW_DONT_CARE);

    const GLFWvidmode* desktop = target != nullptr ? glfwGetVideoMode(target) : nullptr;
    if (mode != WindowMode::Windowed && desktop == nullptr)
    {
        UHE_CORE_WARN("SetWindowMode: monitor {0} reports no video mode", index);
        mode = WindowMode::Windowed;
    }

    switch (mode)
    {
        case WindowMode::Windowed:
        {
            glfwSetWindowAttrib(m_Window, GLFW_DECORATED, GLFW_TRUE);
            glfwSetWindowMonitor(m_Window, nullptr, m_Windowed.X, m_Windowed.Y, restoreWidth, restoreHeight,
                                 GLFW_DONT_CARE);
            if (m_Windowed.Maximized)
                glfwMaximizeWindow(m_Window);
            break;
        }
        case WindowMode::Borderless:
        {
            if (!CanPlaceWindows())
            {
                // Wayland has no exclusive modes: its fullscreen at the desktop
                // mode is a borderless window, and the only way to cover a
                // monitor without a window position.
                glfwSetWindowMonitor(m_Window, target, 0, 0, desktop->width, desktop->height, desktop->refreshRate);
                break;
            }
            // A maximized window keeps the window manager's geometry.
            if (glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED) == GLFW_TRUE)
                glfwRestoreWindow(m_Window);
            int monitorX = 0, monitorY = 0;
            glfwGetMonitorPos(target, &monitorX, &monitorY);
            glfwSetWindowAttrib(m_Window, GLFW_DECORATED, GLFW_FALSE);
            glfwSetWindowMonitor(m_Window, nullptr, monitorX, monitorY, desktop->width, desktop->height,
                                 GLFW_DONT_CARE);
            break;
        }
        case WindowMode::Fullscreen:
        {
            const bool keepMode = width == 0 || height == 0;
            const int fullWidth = keepMode ? desktop->width : static_cast<int>(width);
            const int fullHeight = keepMode ? desktop->height : static_cast<int>(height);
            int refresh = GLFW_DONT_CARE;
            if (refreshRate > 0)
                refresh = static_cast<int>(refreshRate);
            else if (keepMode)
                refresh = desktop->refreshRate;
            glfwSetWindowMonitor(m_Window, target, 0, 0, fullWidth, fullHeight, refresh);
            break;
        }
    }
    m_Mode = mode;
}

void WindowsWindow::SetWindowedSize(u32 width, u32 height)
{
    if (width == 0 || height == 0)
        return;
    m_Windowed.Width = static_cast<int>(width);
    m_Windowed.Height = static_cast<int>(height);
    m_Windowed.Maximized = false;
    if (m_Mode != WindowMode::Windowed)
        return;
    if (glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED) == GLFW_TRUE)
        glfwRestoreWindow(m_Window);
    glfwSetWindowSize(m_Window, m_Windowed.Width, m_Windowed.Height);
}

void WindowsWindow::MoveToMonitor(i32 monitor)
{
    GLFWmonitor* target = MonitorAt(monitor);
    if (m_Mode != WindowMode::Windowed || target == nullptr || !CanPlaceWindows())
        return;

    const bool maximized = glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED) == GLFW_TRUE;
    if (maximized)
        glfwRestoreWindow(m_Window);
    int areaX = 0, areaY = 0, areaWidth = 0, areaHeight = 0;
    glfwGetMonitorWorkarea(target, &areaX, &areaY, &areaWidth, &areaHeight);
    int windowWidth = 0, windowHeight = 0;
    glfwGetWindowSize(m_Window, &windowWidth, &windowHeight);
    glfwSetWindowPos(m_Window, areaX + std::max(0, (areaWidth - windowWidth) / 2),
                     areaY + std::max(0, (areaHeight - windowHeight) / 2));
    if (maximized)
        glfwMaximizeWindow(m_Window);
}

std::vector<MonitorInfo> WindowsWindow::GetMonitors() const
{
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    std::vector<MonitorInfo> result;
    result.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i)
    {
        MonitorInfo info;
        const char* name = glfwGetMonitorName(monitors[i]);
        info.Name = name != nullptr ? name : "Monitor " + std::to_string(i + 1);
        glfwGetMonitorPos(monitors[i], &info.X, &info.Y);
        if (const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]))
        {
            info.Width = static_cast<u32>(mode->width);
            info.Height = static_cast<u32>(mode->height);
            info.RefreshRate = static_cast<u32>(mode->refreshRate);
        }
        result.push_back(std::move(info));
    }
    return result;
}

i32 WindowsWindow::GetCurrentMonitor() const
{
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (monitors == nullptr || count <= 0)
        return -1;
    if (GLFWmonitor* fullscreen = glfwGetWindowMonitor(m_Window))
    {
        for (int i = 0; i < count; ++i)
        {
            if (monitors[i] == fullscreen)
                return i;
        }
    }
    if (!CanPlaceWindows())
        return 0;

    // The monitor that holds the largest part of the window, as a window
    // manager decides where a window spanning two monitors belongs.
    int windowX = 0, windowY = 0, windowWidth = 0, windowHeight = 0;
    glfwGetWindowPos(m_Window, &windowX, &windowY);
    glfwGetWindowSize(m_Window, &windowWidth, &windowHeight);
    i32 best = 0;
    long long bestArea = -1;
    for (int i = 0; i < count; ++i)
    {
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        if (mode == nullptr)
            continue;
        int monitorX = 0, monitorY = 0;
        glfwGetMonitorPos(monitors[i], &monitorX, &monitorY);
        const int overlapWidth = std::min(windowX + windowWidth, monitorX + mode->width) - std::max(windowX, monitorX);
        const int overlapHeight =
            std::min(windowY + windowHeight, monitorY + mode->height) - std::max(windowY, monitorY);
        const long long area =
            static_cast<long long>(std::max(overlapWidth, 0)) * static_cast<long long>(std::max(overlapHeight, 0));
        if (area > bestArea)
        {
            bestArea = area;
            best = i;
        }
    }
    return best;
}

std::vector<VideoMode> WindowsWindow::GetVideoModes(i32 monitor) const
{
    std::vector<VideoMode> result;
    GLFWmonitor* target = MonitorAt(monitor);
    if (target == nullptr)
        return result;
    int count = 0;
    const GLFWvidmode* modes = glfwGetVideoModes(target, &count);
    if (modes == nullptr)
        return result;
    result.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i)
    {
        result.push_back({.Width = static_cast<u32>(modes[i].width),
                          .Height = static_cast<u32>(modes[i].height),
                          .RefreshRate = static_cast<u32>(modes[i].refreshRate)});
    }
    return result;
}

} // namespace UHE
