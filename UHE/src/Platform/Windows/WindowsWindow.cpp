#include "uhepch.h"
#include "WindowsWindow.h"
#include <stb_image.h>
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Core/Log.h"
#include "UHE/Events/ApplicationEvent.h"
#include "UHE/Events/KeyEvent.h"
#include "UHE/Events/MouseEvent.h"

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
}

void WindowsWindow::SetVSync(bool enabled) {
  m_Data.VSync = enabled;
}

bool WindowsWindow::IsVSync() const { return m_Data.VSync; }

void WindowsWindow::SetCursorLocked(bool locked) {
  glfwSetInputMode(m_Window, GLFW_CURSOR,
                   locked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

} // namespace UHE
