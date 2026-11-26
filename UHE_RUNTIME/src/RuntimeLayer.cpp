#include "RuntimeLayer.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Core/CommandLine.h"
#include "UHE/Core/input.h"
#include "UHE/Core/mouseButtonCodes.h"
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/UIRenderer.h"
#include "UHE/Renderer3D/SceneRenderer.h"
#include "UHE/Scene/SceneSerializer.h"

using namespace UHE;

namespace
{
/// The folder of the running executable: where an exported build keeps its project.
std::filesystem::path ExecutableFolder()
{
    const std::filesystem::path self = CommandLine::GetExecutablePath();
    return self.empty() ? std::filesystem::current_path() : self.parent_path();
}
} // namespace

RuntimeLayer::RuntimeLayer(std::optional<ProjectInfo> project) : Layer("RuntimeLayer"), m_Project(std::move(project)) {}

std::optional<ProjectInfo> RuntimeLayer::FindProject()
{
    std::filesystem::path root = CommandLine::GetValue("project");
    if (root.empty())
        root = ExecutableFolder();
    if (!Project::IsProject(root))
    {
        UHE_CORE_ERROR("Runtime: no project at '{0}'; pass -project <folder with {1}>", root.string(),
                       Project::kManifestName);
        return std::nullopt;
    }
    ProjectInfo project;
    std::string error;
    if (!Project::Load(root, project, &error))
    {
        UHE_CORE_ERROR("Runtime: could not load the project at '{0}': {1}", root.string(), error);
        return std::nullopt;
    }
    return project;
}

ProjectWindowSettings RuntimeLayer::ResolveWindow(const ProjectInfo* project)
{
    ProjectWindowSettings window = project ? project->Window : ProjectWindowSettings{};
    if (window.Title.empty())
        window.Title = project ? project->Name : std::string("UHE Runtime");
    if (CommandLine::HasFlag("windowed"))
        window.Mode = WindowMode::Windowed;
    else if (CommandLine::HasFlag("borderless"))
        window.Mode = WindowMode::Borderless;
    else if (CommandLine::HasFlag("fullscreen"))
        window.Mode = WindowMode::Fullscreen;
    const auto size = [](const char* name, u32 fallback)
    {
        const std::string text = CommandLine::GetValue(name);
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (text.empty() || *end != '\0')
            return fallback;
        return static_cast<u32>(
            std::clamp<long>(value, ProjectWindowSettings::kMinSize, ProjectWindowSettings::kMaxSize));
    };
    const std::string vsync = CommandLine::GetValue("vsync");
    if (vsync == "on" || vsync == "1" || vsync == "true")
        window.VSync = true;
    else if (vsync == "off" || vsync == "0" || vsync == "false")
        window.VSync = false;
    window.Width = size("width", window.Width);
    window.Height = size("height", window.Height);
    return window;
}

bool RuntimeLayer::LoadScene(const std::filesystem::path& path)
{
    Ref<Scene> scene = CreateRef<Scene>();
    SceneSerializer serializer(scene);
    if (!serializer.Deserialize(path.string()))
    {
        UHE_CORE_ERROR("Runtime: could not load the scene '{0}'", path.string());
        return false;
    }
    if (m_Scene)
        m_Scene->OnRuntimeStop();
    m_Scene = scene;
    m_Scene->OnViewportResize(m_Width, m_Height);
    m_Scene->OnRuntimeStart();
    UHE_CORE_INFO("Runtime: playing '{0}'", path.filename().string());
    return true;
}

void RuntimeLayer::OnAttach()
{
    Window& window = Application::Get().GetWindow();
    // The window was created at the project's size (RuntimeApp); the mode and
    // VSync are applied once it exists.
    const ProjectWindowSettings settings = ResolveWindow(m_Project ? &*m_Project : nullptr);
    window.SetVSync(settings.VSync);
    if (settings.Mode != WindowMode::Windowed)
        window.SetWindowMode(settings.Mode);
    m_Width = window.GetWidth();
    m_Height = window.GetHeight();
    if (!m_Project)
        return;
    Project::Mount(*m_Project, FileSystem::Get().GetRootPath() / "assets");
    UHE_CORE_INFO("Runtime: project '{0}', {1}x{2} {3}, VSync {4}", m_Project->Name, settings.Width, settings.Height,
                  WindowModeName(settings.Mode), settings.VSync ? "on" : "off");

    std::filesystem::path scenePath = CommandLine::GetValue("scene");
    if (scenePath.empty())
        scenePath = Project::StartScenePath(*m_Project);
    else if (scenePath.is_relative())
        scenePath = m_Project->RootPath / scenePath;
    if (scenePath.empty())
    {
        UHE_CORE_ERROR("Runtime: project '{0}' has no StartScene; set one in Project.uheproject", m_Project->Name);
        return;
    }
    LoadScene(scenePath);
}

void RuntimeLayer::OnDetach()
{
    if (m_Scene)
        m_Scene->OnRuntimeStop();
    m_Scene.reset();
}

void RuntimeLayer::OnUpdate(Timestep ts)
{
    if (!m_Scene)
        return;
    m_Scene->OnUpdateRuntimeSimulation(ts);
    m_Time += ts.GetSeconds();
    DeclareFrame(ts);
}

void RuntimeLayer::DeclareFrame(Timestep ts)
{
    using namespace RHI;
    RHIDevice& device = Renderer::GetDevice();
    FrameGraph* graph = device.GetFrameGraph();
    if (graph == nullptr)
        return;
    const FGTexture::Desc backbuffer = device.GetBackbufferDesc();
    if (backbuffer.Width == 0 || backbuffer.Height == 0)
        return; // minimized

    SceneRenderer::Desc desc;
    desc.Width = backbuffer.Width;
    desc.Height = backbuffer.Height;
    desc.SceneData = m_Scene;
    desc.Runtime = true;
    desc.Lit = true;
    desc.Time = m_Time;
    desc.ColorName = "Runtime.Color";
    desc.EntityName = "Runtime.Entity";
    desc.Target = graph->ImportBackbuffer(backbuffer);
    desc.TargetFormat = backbuffer.Format;
    desc.FlipTarget = true;
    // No sky is drawn yet: the background is the Environment's sky colour
    // (linear, since the target it clears is the HDR image).
    const glm::vec3 sky = m_Scene->GetRenderSettings().Environment.SkyColor;
    const auto linear = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    desc.ClearColor = {linear(sky.r), linear(sky.g), linear(sky.b), 1.0f};
    desc.ResolveView = [scene = m_Scene]()
    {
        if (std::optional<SceneRenderView> view = scene->GetPrimaryCameraView())
            return *view;
        // No camera in the scene: a view from above and behind the origin, so
        // the frame is not empty and the missing camera is obvious.
        SceneRenderView view;
        view.Position = glm::vec3(0.0f, 5.0f, 12.0f);
        view.View = glm::lookAt(view.Position, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        view.Projection = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
        return view;
    };
    desc.DrawScene = [scene = m_Scene, ts](FGContext&) { scene->RenderRuntimeScene(ts); };
    const SceneRenderer::Output output = SceneRenderer::AddPasses(*graph, desc);

    // The game UI over the finished frame, upright in the backbuffer.
    UpdateUI(backbuffer.Width, backbuffer.Height);
    if (output.Color.IsValid())
        UIRenderer::AddPass(*graph, output.Color, backbuffer.Format, backbuffer.Width, backbuffer.Height, false,
                            m_Scene->GetUIDrawList());
}

void RuntimeLayer::UpdateUI(u32 width, u32 height)
{
    // Window pixels; the backbuffer can differ on a scaled display, so scale to it.
    const Window& window = Application::Get().GetWindow();
    const auto [mx, my] = Input::GetMousePosition();
    const float sx = window.GetWidth() > 0 ? static_cast<float>(width) / static_cast<float>(window.GetWidth()) : 1.0f;
    const float sy =
        window.GetHeight() > 0 ? static_cast<float>(height) / static_cast<float>(window.GetHeight()) : 1.0f;
    const bool down = Input::IsMouseButtonPressed(UHE_MOUSE_BUTTON_LEFT);
    UI::Pointer pointer;
    pointer.Position = glm::vec2(mx * sx, my * sy);
    pointer.Active = mx >= 0.0f && my >= 0.0f && mx < static_cast<float>(window.GetWidth()) &&
                     my < static_cast<float>(window.GetHeight());
    pointer.Down = down;
    pointer.Pressed = down && !m_MouseWasDown;
    pointer.Released = !down && m_MouseWasDown;
    m_MouseWasDown = down;
    m_Scene->UpdateUI(glm::vec2(static_cast<float>(width), static_cast<float>(height)), pointer);
}

void RuntimeLayer::OnEvent(Event& event)
{
    EventDispatcher dispatcher(event);
    dispatcher.Dispatch<WindowResizeEvent>(
        [this](WindowResizeEvent& e)
        {
            m_Width = e.GetWidth();
            m_Height = e.GetHeight();
            if (m_Scene && m_Width > 0 && m_Height > 0)
                m_Scene->OnViewportResize(m_Width, m_Height);
            return false;
        });
    dispatcher.Dispatch<KeyPressedEvent>(
        [](KeyPressedEvent& e)
        {
            // Escape quits: a shipped build has no other way out in fullscreen.
            if (e.GetKeyCode() == Key::Escape)
                Application::Get().Close();
            return false;
        });
}
