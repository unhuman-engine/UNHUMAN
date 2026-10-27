#include "Editor.h"
#include "EditorTheme.h"
#include "EditorUI.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "ImGuizmo.h"
#include "UHE/AssestsManager/VfsSystem.h"
#include "UHE/Math/Math.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/Renderer3D/Renderer3D.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraph.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphExecutor.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

ImVec2 m_ViewportPos;
ImVec2 m_ViewportSize;

namespace UHE
{

Editor::Editor()
    : Layer("Editor"), m_CameraController(1280.0f / 720.0f), m_Transform(0.0f, 0.0f, 0.0f),
      m_SceneHireacyPanel(m_ActiveScene), m_ContentBrowserPanel()
{
}
void Editor::OnAttach()
{
    EditorTheme::LoadSelected(); // pick up the persisted theme before first frame

    FramebufferSpecification fbSpec;
    fbSpec.Attachments = {FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER,
                          FramebufferTextureFormat::Depth};
    fbSpec.Width = 1280;
    fbSpec.Height = 720;
    m_Framebuffer = Framebuffer::Create(fbSpec);

    m_ActiveScene = CreateRef<Scene>();
    m_EditorCamera = EditorCamera(45.0f, 1.766f, 0.1f, 1000.0f);
    m_SceneHireacyPanel.SetContext(m_ActiveScene);

    auto rootPath = FileSystem::Get().GetRootPath();
    m_IconPlay = Texture2D::Create((rootPath / "assets/icon/play.png").string());
    m_IconPause = Texture2D::Create((rootPath / "assets/icon/pause.png").string());
    m_IconStop = Texture2D::Create((rootPath / "assets/icon/stop.png").string());
}

void Editor::OnDetach() {}

void Editor::OnUpdate(UHE::Timestep ts)
{
    UHE_PROFILE_FUNCTION();
    m_CameraController.OnUpdate(ts);
    if (FramebufferSpecification spec = m_Framebuffer->GetSpecification();
        m_ViewPortSize.x > 0.0f && m_ViewPortSize.y > 0.0f && // zero sized framebuffer is invalid
        (spec.Width != m_ViewPortSize.x || spec.Height != m_ViewPortSize.y))
    {
        m_Framebuffer->Resize((uint32_t)m_ViewPortSize.x, (uint32_t)m_ViewPortSize.y);
        m_CameraController.OnResize(m_ViewPortSize.x, m_ViewPortSize.y);
        m_EditorCamera.SetViewportSize(m_ViewPortSize.x, m_ViewPortSize.y);
        m_ActiveScene->OnViewportResize((uint32_t)m_ViewPortSize.x, (uint32_t)m_ViewPortSize.y);
        m_FramesSinceResize = 0;
    }
    else
    {
        m_FramesSinceResize++;
    }

    auto& device = Renderer::GetDevice();
    auto* vulkanDevice = dynamic_cast<RHI::VULKAN::VulkanDevice*>(&device);
    const bool graphMode = m_RenderPath == RenderPath::RenderGraph && vulkanDevice != nullptr;
    const bool runtimeScene = m_SceneState != SceneState::Edit;

    auto& cmd = device.GetCurrentCommandBuffer();
    RHI::RenderPassDesc passDesc{};
    if (!graphMode)
    {
        passDesc.renderWidth = m_Framebuffer->GetSpecification().Width;
        passDesc.renderHeight = m_Framebuffer->GetSpecification().Height;

        RHI::ColorAttachment colorAttachment{};
        colorAttachment.texture = m_Framebuffer->GetColorAttachments()[0];
        colorAttachment.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
        passDesc.colorAttachments[0] = colorAttachment;

        RHI::ColorAttachment entityAttachment{};
        entityAttachment.texture = m_Framebuffer->GetColorAttachments()[1];
        entityAttachment.clearColor = {-1.0f, -1.0f, -1.0f, -1.0f}; // Clear to -1
        passDesc.colorAttachments[1] = entityAttachment;

        passDesc.colorAttachmentCount = 2;

        passDesc.hasDepth = true;
        passDesc.depthAttachment.texture = m_Framebuffer->GetDepthAttachment();

        cmd.BeginRenderPass(passDesc);
        cmd.SetViewport(0.0f, 0.0f, (float)passDesc.renderWidth, (float)passDesc.renderHeight);
        cmd.SetScissor(0, 0, passDesc.renderWidth, passDesc.renderHeight);
    }

    switch (m_SceneState)
    {
        case SceneState::Edit:
        {
            if (m_ViewPortFocused)
            {
                m_CameraController.OnUpdate(ts);
            }
            m_EditorCamera.OnUpdate(ts);

            if (graphMode)
                m_ActiveScene->OnUpdateEditorSimulation(ts);
            else
                m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
            break;
        }
        case SceneState::Play:
        {
            if (graphMode)
                m_ActiveScene->OnUpdateRuntimeSimulation(ts);
            else
                m_ActiveScene->OnUpdateRuntime(ts);
            break;
        }
        case SceneState::Paused:
        {
            // Let the editor camera fly around while the simulation is frozen.
            m_EditorCamera.OnUpdate(ts);
            if (m_StepRequested)
            {
                if (graphMode)
                    m_ActiveScene->OnUpdateRuntimeSimulation(ts);
                else
                    m_ActiveScene->OnUpdateRuntime(ts);
                m_StepRequested = false;
            }
            break;
        }
    }

    if (graphMode)
    {
        // Scene rendering is deferred into EndFrameGraph so the compiler owns
        // the attachment transitions; the ImGui pass is declared after this
        // and therefore runs second (UI on top).
        DeclareSceneGraphPass(ts, runtimeScene);
    }
    else
    {
        cmd.EndRenderPass();
    }

    // The swapchain scope belongs to the frame graph now (§14 step 5): the
    // ImGui pass declares it Undefined→ColorAttachment→Present and the
    // executor owns that scope. The legacy empty-swapchain Begin/EndRenderPass
    // here cleared the frame, fought the graph's Load-based scope, and double-
    // transitioned the image to Present before the graph pass ran.
}

void Editor::DeclareSceneGraphPass(Timestep ts, bool runtime)
{
    using RHI::LoadOp;
    using RHI::StoreOp;
    using RHI::VULKAN::ImageState;
    using RHI::VULKAN::RGPassContext;
    using RHI::VULKAN::RGPassType;
    using RHI::VULKAN::RGTextureHandle;

    auto* vulkanDevice = dynamic_cast<RHI::VULKAN::VulkanDevice*>(&Renderer::GetDevice());
    if (!vulkanDevice)
        return;

    auto& graph = vulkanDevice->GetFrameGraph();
    const auto& spec = m_Framebuffer->GetSpecification();
    const u32 width = spec.Width;
    const u32 height = spec.Height;

    // Imported every frame: framebuffer textures are recreated on resize, and
    // Reset() bumps generations, so cached handles would be stale.
    RGTextureHandle color = graph.ImportTexture("Editor.Scene.Color", 1, width, height, 1,
                                                ImageState::Undefined, ImageState::ShaderRead);
    RGTextureHandle entity = graph.ImportTexture("Editor.Scene.Entity", 2, width, height, 1,
                                                 ImageState::Undefined, ImageState::ShaderRead);
    RGTextureHandle depth = graph.ImportTexture("Editor.Scene.Depth", 3, width, height, 1,
                                                ImageState::Undefined, ImageState::Undefined);

    auto& pass = graph.AddPass("Editor.Scene", RGPassType::Graphics);
    pass.Write(color)
        .Write(entity)
        .Write(depth)
        .Color({color, LoadOp::Clear, StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}})
        .Color({entity, LoadOp::Clear, StoreOp::Store, {-1.0f, -1.0f, -1.0f, -1.0f}})
        .Depth({depth, LoadOp::Clear, StoreOp::Store, 1.0f, 0})
        .Execute([this, ts, runtime](RGPassContext& context)
                 { RecordSceneGraphPass(context, ts, runtime); });

    vulkanDevice->RegisterFrameTexture(color, m_Framebuffer->GetColorAttachments()[0]);
    vulkanDevice->RegisterFrameTexture(entity, m_Framebuffer->GetColorAttachments()[1]);
    vulkanDevice->RegisterFrameTexture(depth, m_Framebuffer->GetDepthAttachment());
}

void Editor::RecordSceneGraphPass(RHI::VULKAN::RGPassContext& context, Timestep ts, bool runtime)
{
    const vk::Extent2D extent = context.Extent();
    if (extent.width == 0 || extent.height == 0)
        return;

    const vk::CommandBuffer cmd = context.Cmd();
    const vk::Viewport viewport{0.0f, 0.0f, static_cast<f32>(extent.width), static_cast<f32>(extent.height),
                                0.0f, 1.0f};
    cmd.setViewport(0, viewport, context.Dispatcher());
    cmd.setScissor(0, vk::Rect2D{vk::Offset2D{0, 0}, extent}, context.Dispatcher());

    if (runtime)
        m_ActiveScene->RenderRuntimeScene(ts);
    else
        m_ActiveScene->RenderEditorScene(ts, m_EditorCamera);
}

void Editor::OnEvent(UHE::Event& e)
{
    m_CameraController.OnEvent(e);
    m_EditorCamera.OnEvent(e);

    EventDispatcher dispatcher(e);
    dispatcher.Dispatch<MouseButtonPressedEvent>(UHE_BIND_EVENT_FN(Editor::OnMouseButtonPressedEvent));
    dispatcher.Dispatch<KeyPressedEvent>(UHE_BIND_EVENT_FN(Editor::onKeyPressed));
}

void Editor::OnScreenPlay()
{
    if (m_SceneState == SceneState::Edit)
    {
        m_HoverdEntity = {};
        m_EditorScene = m_ActiveScene;
        m_ActiveScene = Scene::Copy(m_EditorScene);
        m_ActiveScene->OnRuntimeStart();
        m_SceneState = SceneState::Play;
    }
    else if (m_SceneState == SceneState::Paused)
    {
        m_SceneState = SceneState::Play;
    }
}

void Editor::OnScreenPause()
{
    if (m_SceneState == SceneState::Play)
        m_SceneState = SceneState::Paused;
    else if (m_SceneState == SceneState::Paused)
        m_SceneState = SceneState::Play;
}

void Editor::OnScreenStep()
{
    if (m_SceneState == SceneState::Paused)
        m_StepRequested = true;
}

void Editor::OnSceneStop()
{
    if (m_SceneState == SceneState::Edit)
        return;

    m_HoverdEntity = {};
    m_StepRequested = false;
    m_ActiveScene->OnRuntimeStop();
    m_SceneState = SceneState::Edit;

    m_ActiveScene = m_EditorScene;
    m_SceneHireacyPanel.SetContext(m_ActiveScene);
}

bool Editor::onKeyPressed(KeyPressedEvent& e)
{
    if (e.GetRepeatCount() > 0)
        return false;

    bool controlPressed = (Input::IsKeyPressed(Key::LeftControl) || Input::IsKeyPressed(Key::RightControl));
    bool shift = (Input::IsKeyPressed(Key::LeftShift) || Input::IsKeyPressed(Key::RightShift));
    switch (e.GetKeyCode())
    {
        case Key::N:
        {
            if (controlPressed)
            {
                NewScene();
            }
            break;
        }
        case Key::F:
        {
            if (controlPressed && shift)
            {
                m_IsViewportFullScreen = !m_IsViewportFullScreen;
            }
            break;
        }
        case Key::O:
        {
            if (controlPressed)
                OpenScene();

            break;
        }
        case Key::S:
        {
            if (controlPressed)
                SaveSceneAs();
            break;
        }
        case Key::F5:
            if (m_SceneState == SceneState::Edit)
                OnScreenPlay();
            else
                OnSceneStop();
            break;
        case Key::F6:
            OnScreenPause();
            break;
        case Key::F7:
            OnScreenStep();
            break;
        case Key::F1:
            m_ShowAbout = true;
            break;
        case Key::Comma:
            if (controlPressed)
                m_ShowPreferences = true;
            break;
        case Key::Q:
            m_GizmoType = -1;
            break;
        case Key::W:
            m_GizmoType = ImGuizmo::OPERATION::TRANSLATE;
            break;
        case Key::E:
            m_GizmoType = ImGuizmo::OPERATION::ROTATE;
            break;
        case Key::R:
            m_GizmoType = ImGuizmo::OPERATION::SCALE;
            break;
    }
    return false;
}

void Editor::NewScene()
{
    OnSceneStop();
    m_ActiveScene = CreateRef<Scene>();
    m_ActiveScene->OnViewportResize((u32)m_ViewPortSize.x, (u32)m_ViewPortSize.y);
    m_SceneHireacyPanel.SetContext(m_ActiveScene);
    m_CurrentScenePath.clear();
}

void Editor::OpenScene()
{
    std::string filepath = FileDialogs::OpenFile("UHE Scene (*.unhuman)\0*.unhuman\0");
    if (!filepath.empty())
    {
        OpenScene(filepath);
    }
}

void Editor::OpenScene(const std::filesystem::path& path)
{
    std::string filepath = path.string();

    if (!filepath.empty())
    {
        OnSceneStop();
        m_ActiveScene = CreateRef<Scene>();
        m_ActiveScene->OnViewportResize((u32)m_ViewPortSize.x, (u32)m_ViewPortSize.y);
        m_SceneHireacyPanel.SetContext(m_ActiveScene);
        SceneSerializer serializer(m_ActiveScene);
        serializer.Deserialize(filepath);
        m_CurrentScenePath = path;
    }
}

void DrawConsolePanel()
{
    UHE_PROFILE_FUNCTION();
    ImGui::Begin("Console");

    auto sink = UHE::Log::GetImGuiSink();
    std::vector<UHE::ImGuiLogEntry> logs = sink->GetSnapshot();

    static constexpr int kLevelCount = 6;
    static const char* kLevelNames[kLevelCount] = {"Trace", "Debug", "Info", "Warn", "Error", "Critical"};
    static bool s_LevelEnabled[kLevelCount] = {true, true, true, true, true, true};
    static char s_Search[256] = "";
    static bool s_AutoScroll = true;

    auto levelIndex = [](spdlog::level::level_enum level) -> int
    {
        switch (level)
        {
            case spdlog::level::trace:    return 0;
            case spdlog::level::debug:    return 1;
            case spdlog::level::info:     return 2;
            case spdlog::level::warn:     return 3;
            case spdlog::level::err:      return 4;
            case spdlog::level::critical: return 5;
            default:                      return 2;
        }
    };

    size_t counts[kLevelCount] = {};
    for (const auto& log : logs)
        counts[levelIndex(log.Level)]++;

    // Theme-aware console palette (readable on light themes too).
    ImVec4 debugColor = EditorTheme::ConsoleInfo();
    debugColor.w *= 0.65f;
    const ImVec4 colors[kLevelCount] = {
        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
        debugColor,
        EditorTheme::ConsoleInfo(),
        EditorTheme::ConsoleWarn(),
        EditorTheme::ConsoleError(),
        EditorTheme::ConsoleCritical(),
    };

    // A needle lowercase once instead of per-message.
    auto toLower = [](std::string value)
    {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    const std::string needle = toLower(s_Search);

    auto isVisible = [&](const UHE::ImGuiLogEntry& entry)
    {
        if (!s_LevelEnabled[levelIndex(entry.Level)])
            return false;
        if (!needle.empty() && toLower(entry.Message).find(needle) == std::string::npos)
            return false;
        return true;
    };

    // Level filters with per-level counters.
    for (int i = 0; i < kLevelCount; i++)
    {
        if (i > 0)
            ImGui::SameLine();
        char label[64];
        std::snprintf(label, sizeof(label), "%s (%zu)", kLevelNames[i], counts[i]);
        ImGui::PushStyleColor(ImGuiCol_Text, colors[i]);
        ImGui::Checkbox(label, &s_LevelEnabled[i]);
        ImGui::PopStyleColor();
    }

    if (ImGui::Button("Clear"))
        sink->Clear();
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &s_AutoScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##ConsoleFilter", "Filter text...", s_Search, sizeof(s_Search));

    std::vector<const UHE::ImGuiLogEntry*> visible;
    visible.reserve(logs.size());
    for (const auto& log : logs)
        if (isVisible(log))
            visible.push_back(&log);

    ImGui::SameLine();
    ImGui::BeginDisabled(visible.empty());
    if (ImGui::Button("Copy"))
    {
        std::string text;
        for (const auto* entry : visible)
        {
            text += entry->Message;
            if (text.empty() || text.back() != '\n')
                text += '\n';
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu", visible.size(), logs.size());
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Log file: %s", UHE::Log::GetLogFilePath().c_str());

    ImGui::Separator();

    ImGui::BeginChild("##ConsoleLogRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto* entry : visible)
        ImGui::TextColored(colors[levelIndex(entry->Level)], "%s", entry->Message.c_str());

    // Auto-scroll only when already pinned to the bottom, so inspecting older
    // lines is not interrupted by incoming logs.
    if (s_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::End();
}

void Editor::SaveSceneAs()
{
    std::string filepath = FileDialogs::SaveFile("UHE Scene (*.unhuman)\0.unhuman\0");

    if (!filepath.empty())
    {
        SceneSerializer serializer(m_ActiveScene);
        serializer.Serialize(filepath);
        m_CurrentScenePath = filepath;
    }
}

namespace
{
    void BuildDefaultLayout(ImGuiID dockspaceId, const ImVec2& size)
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, size);

        ImGuiID left = 0, right = 0;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.21f, &left, &right);

        ImGuiID leftTop = 0, leftBottom = 0;
        ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.68f, &leftTop, &leftBottom);

        ImGuiID inspector = 0, center = 0;
        ImGui::DockBuilderSplitNode(right, ImGuiDir_Right, 0.27f, &inspector, &center);

        ImGuiID bottom = 0, centerTop = 0;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, &bottom, &centerTop);

        ImGuiID bottomLeft = 0, bottomRight = 0;
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.50f, &bottomLeft, &bottomRight);

        ImGui::DockBuilderDockWindow("Scene Hierarchy", leftTop);
        ImGui::DockBuilderDockWindow("Settings", leftBottom);
        ImGui::DockBuilderDockWindow("Properties", inspector);
        ImGui::DockBuilderDockWindow("Viewport", centerTop);
        ImGui::DockBuilderDockWindow("Content Browser", bottomLeft);
        ImGui::DockBuilderDockWindow("Console", bottomRight);
        ImGui::DockBuilderDockWindow("Profiler", bottomRight);
        ImGui::DockBuilderFinish(dockspaceId);
    }
} // namespace

void Editor::OnImGuiRender()
{
    UHE_PROFILE_FUNCTION();
    EditorTheme::EnsureApplied();

    static bool dockspaceOpen = true;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("UHE Editor", &dockspaceOpen, window_flags);
    ImGui::PopStyleVar(3);

    DrawMainMenuBar();
    DrawEditorToolbar();

    ImGuiStyle& style = ImGui::GetStyle();
    const f32 previousMinWidth = style.WindowMinSize.x;
    style.WindowMinSize.x = 220.0f;

    const ImGuiID dockspaceId = ImGui::GetID("UHE_DockSpace_v2");
    if (m_ResetLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
    {
        BuildDefaultLayout(dockspaceId, ImGui::GetContentRegionAvail());
        m_ResetLayout = false;
    }

    const f32 statusBarHeight = ImGui::GetFrameHeight() + 6.0f;
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -statusBarHeight), ImGuiDockNodeFlags_None);
    style.WindowMinSize.x = previousMinWidth;

    DrawStatusBar();
    ImGui::End();

    DrawPreferencesModal();
    DrawAboutModal();

    if (m_ShowHierarchy || m_ShowInspector)
        m_SceneHireacyPanel.OnImGuiRender(m_ShowHierarchy, m_ShowInspector);
    m_SceneHireacyPanel.ApplyQueuedMutations(); // Issue #17: apply tree edits outside registry iteration

    if (m_ShowContentBrowser)
        m_ContentBrowserPanel.OnImGuiRender();

    if (m_ShowSettings)
    {
        ImGui::Begin("Settings");
        DrawPreferencesContent();
        ImGui::End();
    }

    if (m_ShowConsole)
        DrawConsolePanel();

    if (m_ShowProfiler)
        m_ProfilerPanel.OnImGuiRender();

    ImGuiWindowFlags viewportFlags = 0;
    std::string viewportName = "Viewport";
    const bool fullscreen = m_IsViewportFullScreen;

    if (fullscreen)
    {
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        viewportFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;
        viewportName = "FullscreenViewport";
    }

    if (m_ShowViewport)
    {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
    ImGui::Begin(viewportName.c_str(), nullptr, viewportFlags);

    if (fullscreen)
    {
        ImGui::SetWindowFocus();
    }

    if (!fullscreen)
        DrawViewportHeader();

    m_ViewPortFocused = ImGui::IsWindowFocused();
    m_ViewPortHover = ImGui::IsWindowHovered();
    Application::Get().GetImguiLayer()->SetBlockEvent(!m_ViewPortFocused && !m_ViewPortHover);

    ImVec2 viewPortSize = ImGui::GetContentRegionAvail();
    if (viewPortSize.x < 1.0f)
        viewPortSize.x = 1.0f;
    if (viewPortSize.y < 1.0f)
        viewPortSize.y = 1.0f;
    m_CameraController.OnResize(viewPortSize.x, viewPortSize.y);
    m_ActiveScene->OnViewportResize(static_cast<u32>(viewPortSize.x), static_cast<u32>(viewPortSize.y));

    m_ViewPortSize = {viewPortSize.x, viewPortSize.y};
    const ImVec2 imageMin = ImGui::GetCursorScreenPos();
    void* textureId = m_Framebuffer->GetColorAttachmentRendererID(0);

    ImGui::Image(textureId, viewPortSize, ImVec2{0, 1}, ImVec2{1, 0});
    const ImVec2 imageMax = ImGui::GetItemRectMax();

    m_ViewportPos = {imageMin.x, imageMin.y};
    m_ViewPortBounds[0] = {imageMin.x, imageMin.y};
    m_ViewPortBounds[1] = {imageMax.x, imageMax.y};

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("Content_Browser_item"))
        {
            const char* path = (const char*)payload->Data;
            std::filesystem::path itemPath(path);
            std::string extension = itemPath.extension().string();

            if (extension == ".unhuman")
            {
                OpenScene(path);
            }
            else if (extension == ".png" || extension == ".jpg")
            {
                UHE_CORE_INFO("Accepted Viewport DragDropPayload: {0}", path);
                // Create a new entity with the texture
                std::string filename = itemPath.stem().string();
                Entity spriteEntity = m_ActiveScene->CreateEntity(filename);
                auto& src = spriteEntity.AddComponent<SpriteRendererComponent>();
                src.TexturePath = itemPath.string();
                src.Texture = Texture2D::Create(itemPath.string());
                if (src.Texture)
                {
                    UHE_CORE_INFO("Successfully loaded texture into viewport!");
                }
                else
                {
                    UHE_CORE_ERROR("Failed to load texture into viewport from payload!");
                }
            }
            else if (extension == ".gltf" || extension == ".glb")
            {
                UHE_CORE_INFO("Accepted Viewport DragDropPayload (3D Model): {0}", path);
                std::string filename = itemPath.stem().string();
                Entity modelEntity = m_ActiveScene->CreateEntity(filename);
                auto& mc = modelEntity.AddComponent<Model3DComponent>();
                mc.ModelPath = path;
                if (mc.ModelData->loadModel(itemPath.string()))
                {
                    mc.IsLoaded = true;
                    UHE_CORE_INFO("Successfully loaded 3D model into viewport!");
                }
                else
                {
                    mc.IsLoaded = false;
                    UHE_CORE_ERROR("Failed to load 3D model into viewport from payload!");
                }
            }
        }

        ImGui::EndDragDropTarget();
    }

    auto [mx, my] = ImGui::GetMousePos();
    mx -= imageMin.x;
    my -= imageMin.y;
    const glm::vec2 viewportSize = {imageMax.x - imageMin.x, imageMax.y - imageMin.y};
    int mouseX = (int)mx;
    int mouseY = (int)my;

    if (mouseX >= 0 && mouseY >= 0 && mouseX < (int)viewportSize.x && mouseY < (int)viewportSize.y)
    {
        if (m_FramesSinceResize > 1)
        {
            int pixelData = m_Framebuffer->ReadPixel(1, mouseX, mouseY);
            if (pixelData == -1 || pixelData < 0)
            {
                m_HoverdEntity = Entity();
            }
            else
            {
                entt::entity entId = (entt::entity)pixelData;
                if (m_ActiveScene->GetRegistry().valid(entId))
                {
                    m_HoverdEntity = Entity(entId, m_ActiveScene.get());
                }
                else
                {
                    m_HoverdEntity = Entity();
                }
            }
        }
    }

    // Gizmos
    Entity SelectedEntity = m_SceneHireacyPanel.GetSelectedEntity();

    if (SelectedEntity)
    {

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(imageMin.x, imageMin.y, imageMax.x - imageMin.x, imageMax.y - imageMin.y);

        if (m_GizmoType != -1)
        {
            glm::mat4 cameraView = m_EditorCamera.GetViewMatrix();
            glm::mat4 cameraprojection = m_EditorCamera.GetProjection();

            glm::mat4 transform = glm::mat4(1.0f);
            if (SelectedEntity.HasComponent<TransformComponent>())
            {
                // Issue #17: gizmos operate on the world transform so entities
                // nested under a parent still line up with what is on screen.
                transform = m_ActiveScene->GetWorldSpaceTransformMatrix(SelectedEntity);
            }

            bool snap = m_SnapEnabled || Input::IsKeyPressed(Key::LeftControl);
            f32 snapValue = 0.5f;

            if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
                snapValue = 45.0f;

            f32 snapValues[3] = {snapValue, snapValue, snapValue};

            ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraprojection),
                                 (ImGuizmo::OPERATION)m_GizmoType,
                                 m_GizmoSpaceWorld ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
                                 glm::value_ptr(transform), nullptr, snap ? snapValues : nullptr);

            if (ImGuizmo::IsUsing() && SelectedEntity.HasComponent<TransformComponent>())
            {
                // Issue #17: convert the manipulated world transform back to
                // local space relative to the entity's parent.
                glm::mat4 local = transform;
                Entity parent = m_ActiveScene->GetParentEntity(SelectedEntity);
                if (parent)
                    local = glm::inverse(m_ActiveScene->GetWorldSpaceTransformMatrix(parent)) * transform;

                glm::vec3 translation, rotation, scale;
                Math::DecomposeTransform(local, translation, rotation, scale);
                auto& tc = SelectedEntity.GetComponent<TransformComponent>();
                tc.Translation = translation;
                tc.Rotation = rotation;
                tc.Scale = scale;
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
    }

}

bool Editor::OnMouseButtonPressedEvent(MouseButtonPressedEvent& e)
{
    if (e.GetMouseButton() == UHE_MOUSE_BUTTON_LEFT)
    {
        if (m_ViewPortHover && !ImGuizmo::IsOver() && !Input::IsKeyPressed(Key::LeftAlt))
        {
            m_SceneHireacyPanel.SetSelectedEntity(m_HoverdEntity);
        }
    }
    return false;
}

void Editor::DrawMainMenuBar()
{
    UHE_PROFILE_FUNCTION();
    if (!ImGui::BeginMenuBar())
        return;

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Scene", "Ctrl+N"))
            NewScene();
        if (ImGui::MenuItem("Open Scene...", "Ctrl+O"))
            OpenScene();
        if (ImGui::MenuItem("Save Scene As...", "Ctrl+S"))
            SaveSceneAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4"))
            Application::Get().Close();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        if (ImGui::MenuItem("Preferences...", "Ctrl+,"))
            m_ShowPreferences = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Create"))
    {
        ImGui::BeginDisabled(m_SceneState != SceneState::Edit);
        if (ImGui::MenuItem("Empty Entity"))
            CreateEntityFromMenu("Empty Entity", 0);
        ImGui::Separator();
        if (ImGui::MenuItem("Camera"))
            CreateEntityFromMenu("Camera", 1);
        if (ImGui::MenuItem("Sprite"))
            CreateEntityFromMenu("Sprite", 2);
        if (ImGui::MenuItem("Text"))
            CreateEntityFromMenu("Text", 3);
        ImGui::Separator();
        if (ImGui::MenuItem("Directional Light"))
            CreateEntityFromMenu("Directional Light", 4);
        if (ImGui::MenuItem("Point Light"))
            CreateEntityFromMenu("Point Light", 5);
        if (ImGui::MenuItem("Spot Light"))
            CreateEntityFromMenu("Spot Light", 6);
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        EditorUI::ToggleMenuItem("Hierarchy", &m_ShowHierarchy);
        EditorUI::ToggleMenuItem("Inspector", &m_ShowInspector);
        EditorUI::ToggleMenuItem("Content Browser", &m_ShowContentBrowser);
        EditorUI::ToggleMenuItem("Console", &m_ShowConsole);
        EditorUI::ToggleMenuItem("Profiler", &m_ShowProfiler);
        EditorUI::ToggleMenuItem("Settings", &m_ShowSettings);
        EditorUI::ToggleMenuItem("Viewport", &m_ShowViewport);
        ImGui::Separator();
        if (ImGui::MenuItem("Fullscreen Viewport", "Ctrl+Shift+F"))
            m_IsViewportFullScreen = !m_IsViewportFullScreen;
        if (ImGui::MenuItem("Reset Layout"))
            m_ResetLayout = true;
        ImGui::Separator();
        if (ImGui::BeginMenu("Theme"))
        {
            for (int i = 0; i < EditorTheme::GetThemeCount(); i++)
            {
                auto id = (EditorTheme::EditorThemeId)i;
                bool selected = EditorTheme::GetSelected() == id;
                if (ImGui::MenuItem(EditorTheme::GetName(id), nullptr, selected) && !selected)
                    EditorTheme::SetSelected(id);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("About UHE"))
            m_ShowAbout = true;
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void Editor::DrawEditorToolbar()
{
    UHE_PROFILE_FUNCTION();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::ToolbarBg());
    ImGui::BeginChild("##EditorToolbar", ImVec2(0, 38.0f), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(8.0f, 4.0f));

    if (EditorUI::ToolIconButton("##SelectTool", EditorUI::ToolIcon::Select, m_GizmoType == -1, "Select (Q)"))
        m_GizmoType = -1;
    ImGui::SameLine(0.0f, 3.0f);
    if (EditorUI::ToolIconButton("##MoveTool", EditorUI::ToolIcon::Move, m_GizmoType == ImGuizmo::TRANSLATE, "Move (W)"))
        m_GizmoType = ImGuizmo::TRANSLATE;
    ImGui::SameLine(0.0f, 3.0f);
    if (EditorUI::ToolIconButton("##RotateTool", EditorUI::ToolIcon::Rotate, m_GizmoType == ImGuizmo::ROTATE, "Rotate (E)"))
        m_GizmoType = ImGuizmo::ROTATE;
    ImGui::SameLine(0.0f, 3.0f);
    if (EditorUI::ToolIconButton("##ScaleTool", EditorUI::ToolIcon::Scale, m_GizmoType == ImGuizmo::SCALE, "Scale (R)"))
        m_GizmoType = ImGuizmo::SCALE;

    ImGui::SameLine(0.0f, 10.0f);
    EditorUI::Separator();
    ImGui::SameLine(0.0f, 10.0f);

    if (EditorUI::ToolbarToggle("World", m_GizmoSpaceWorld, "Toggle world/local gizmo space", ImVec2(58, 28)))
        m_GizmoSpaceWorld = !m_GizmoSpaceWorld;
    ImGui::SameLine(0.0f, 4.0f);
    if (EditorUI::ToolbarToggle("Snap", m_SnapEnabled, "Snap transforms to increments", ImVec2(52, 28)))
        m_SnapEnabled = !m_SnapEnabled;

    const f32 controlsWidth = 28.0f * 3.0f + 6.0f * 2.0f + 10.0f + 48.0f;
    f32 controlsX = (ImGui::GetWindowWidth() - controlsWidth) * 0.5f;
    if (controlsX < ImGui::GetCursorPosX() + 20.0f)
        controlsX = ImGui::GetCursorPosX() + 20.0f;
    ImGui::SameLine(controlsX);

    const bool playing = m_SceneState == SceneState::Play;
    const bool paused = m_SceneState == SceneState::Paused;

    if (EditorUI::TransportButton("##Play", EditorUI::TransportIcon::Play,
                                  m_SceneState != SceneState::Play, playing, "Play (F5)",
                                  ImVec4(0.45f, 0.92f, 0.55f, 1.0f)))
        OnScreenPlay();
    ImGui::SameLine(0.0f, 6.0f);
    if (EditorUI::TransportButton("##Pause", EditorUI::TransportIcon::Pause,
                                  m_SceneState != SceneState::Edit, paused, "Pause (F6)",
                                  ImVec4(1.0f, 0.80f, 0.30f, 1.0f)))
        OnScreenPause();
    ImGui::SameLine(0.0f, 6.0f);
    if (EditorUI::TransportButton("##Stop", EditorUI::TransportIcon::Stop,
                                  m_SceneState != SceneState::Edit, false,
                                  "Stop (F5 while playing)", ImVec4(0.98f, 0.42f, 0.42f, 1.0f)))
        OnSceneStop();

    ImGui::SameLine(0.0f, 10.0f);
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button("Step", ImVec2(48.0f, 28.0f)))
        OnScreenStep();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Advance one frame while paused (F7)");

    const char* stateLabel = "Editing";
    ImVec4 stateColor = EditorTheme::ConsoleInfo();
    if (playing)
    {
        stateLabel = "Playing";
        stateColor = EditorTheme::Success();
    }
    else if (paused)
    {
        stateLabel = "Paused";
        stateColor = EditorTheme::Warning();
    }

    const f32 displayWidth = 78.0f + 4.0f + 88.0f + 12.0f + 1.0f + 12.0f + 62.0f;
    f32 clusterX = ImGui::GetWindowWidth() - 8.0f - displayWidth;
    const f32 minClusterX = ImGui::GetCursorPosX() + 16.0f;
    if (clusterX < minClusterX)
        clusterX = minClusterX;
    ImGui::SameLine(clusterX);
    ImGui::SetCursorPosY(5.0f);

    bool lighting = Renderer3D::IsLightingEnabled();
    if (EditorUI::ToolbarToggle("Lighting", lighting, "Toggle 3D lighting", ImVec2(78.0f, 28.0f)))
        Renderer3D::SetLightingEnabled(!lighting);
    ImGui::SameLine(0.0f, 4.0f);
    if (EditorUI::ToolbarToggle("Fullscreen", m_IsViewportFullScreen,
                                "Toggle fullscreen viewport (Ctrl+Shift+F)", ImVec2(88.0f, 28.0f)))
        m_IsViewportFullScreen = !m_IsViewportFullScreen;

    ImGui::SameLine(0.0f, 12.0f);
    EditorUI::Separator();
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetCursorPosY(10.0f);
    EditorUI::Badge(stateLabel, stateColor);

    ImGui::EndChild();
}

void Editor::DrawViewportHeader()
{
    UHE_PROFILE_FUNCTION();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::ToolbarBg());
    ImGui::BeginChild("##ViewportHeader", ImVec2(0, ImGui::GetFrameHeight() + 6.0f), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(8.0f, 4.0f));

    ImGuiIO& io = ImGui::GetIO();
    ImFont* boldFont = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
    const std::string sceneName = m_CurrentScenePath.empty()
        ? std::string("Untitled Scene")
        : m_CurrentScenePath.filename().string();

    ImGui::TextDisabled("Scene");
    ImGui::SameLine(0.0f, 6.0f);
    EditorUI::Separator();
    ImGui::SameLine(0.0f, 8.0f);

    if (boldFont)
        ImGui::PushFont(boldFont);
    ImGui::TextUnformatted(sceneName.c_str());
    if (boldFont)
        ImGui::PopFont();
    if (ImGui::IsItemHovered() && !m_CurrentScenePath.empty())
        ImGui::SetTooltip("%s", m_CurrentScenePath.string().c_str());

    const char* renderPathLabel = m_RenderPath == RenderPath::RenderGraph ? "Render Graph" : "Immediate";
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(renderPathLabel).x - 10.0f);
    ImGui::TextDisabled("%s", renderPathLabel);

    ImGui::EndChild();
}

void Editor::DrawStatusBar()
{
    UHE_PROFILE_FUNCTION();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::ToolbarBg());
    ImGui::BeginChild("##StatusBar", ImVec2(0, ImGui::GetFrameHeight() + 6.0f), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(8.0f, 3.0f));

    const char* stateLabel = "Editing";
    ImVec4 stateColor = EditorTheme::ConsoleInfo();
    if (m_SceneState == SceneState::Play)
    {
        stateLabel = "Playing";
        stateColor = EditorTheme::Success();
    }
    else if (m_SceneState == SceneState::Paused)
    {
        stateLabel = "Paused";
        stateColor = EditorTheme::Warning();
    }

    EditorUI::Badge(stateLabel, stateColor);
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextUnformatted(m_CurrentScenePath.empty() ? "Untitled Scene" : m_CurrentScenePath.filename().string().c_str());

    ImGui::SameLine(0.0f, 10.0f);
    EditorUI::Separator();
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::TextDisabled("Entities: %zu", m_ActiveScene ? m_ActiveScene->GetRegistry().view<entt::entity>().size() : 0);

    ImGui::SameLine(0.0f, 12.0f);
    if (m_HoverdEntity && m_HoverdEntity.HasComponent<TagComponent>())
        ImGui::TextDisabled("Hover: %s", m_HoverdEntity.GetComponent<TagComponent>().Tag.c_str());
    else
        ImGui::TextDisabled("Hover: -");

    ImGui::SameLine(ImGui::GetWindowWidth() - 190.0f);
    ImGui::TextDisabled("%.0f FPS  |  %.2f ms", ImGui::GetIO().Framerate,
                        1000.0f / (ImGui::GetIO().Framerate > 0.0f ? ImGui::GetIO().Framerate : 1.0f));

    ImGui::EndChild();
}

void Editor::DrawPreferencesContent()
{
    UHE_PROFILE_FUNCTION();
    EditorUI::SectionHeader("Rendering");
    const char* renderPaths[] = {"Immediate", "Render Graph"};
    int renderPath = static_cast<int>(m_RenderPath);
    if (ImGui::Combo("Render Path", &renderPath, renderPaths, 2))
        m_RenderPath = static_cast<RenderPath>(renderPath);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Immediate records directly into the frame command buffer.\n"
                          "Render Graph defers the scene into a graph pass owned by the compiler.");
    bool enableLighting = Renderer3D::IsLightingEnabled();
    if (ImGui::Checkbox("Enable Basic3D Lighting", &enableLighting))
        Renderer3D::SetLightingEnabled(enableLighting);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lighting is shared by Basic3D materials");
    bool showLightIcons = Scene::GetShowLightIcons();
    if (ImGui::Checkbox("Show Light Icons", &showLightIcons))
        Scene::GetShowLightIcons() = showLightIcons;
    ImGui::Checkbox("Snap Transforms", &m_SnapEnabled);
    ImGui::Checkbox("World-space Gizmos", &m_GizmoSpaceWorld);

    EditorUI::SectionHeader("Appearance");
    const char* themeLabel = EditorTheme::GetName(EditorTheme::GetSelected());
    if (ImGui::BeginCombo("Theme", themeLabel))
    {
        for (int i = 0; i < EditorTheme::GetThemeCount(); i++)
        {
            auto id = (EditorTheme::EditorThemeId)i;
            bool selected = EditorTheme::GetSelected() == id;
            if (ImGui::Selectable(EditorTheme::GetName(id), selected) && !selected)
                EditorTheme::SetSelected(id);
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    bool customEnabled = EditorTheme::IsCustomAccentEnabled();
    ImVec4 accent = customEnabled ? EditorTheme::GetCustomAccent() : EditorTheme::Accent();
    ImGui::BeginDisabled(!customEnabled);
    ImGuiColorEditFlags accentFlags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
                                      ImGuiColorEditFlags_AlphaPreviewHalf;
    if (ImGui::ColorEdit3("Accent", &accent.x, accentFlags))
        EditorTheme::SetCustomAccent(accent);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox("Custom Accent", &customEnabled))
        EditorTheme::SetCustomAccentEnabled(customEnabled);
    if (customEnabled && ImGui::Button("Reset Accent"))
        EditorTheme::SetCustomAccentEnabled(false);

    EditorUI::SectionHeader("Layout");
    if (ImGui::Button("Reset Layout"))
        m_ResetLayout = true;
    ImGui::SameLine();
    ImGui::Checkbox("Fullscreen Viewport", &m_IsViewportFullScreen);

    EditorUI::SectionHeader("About");
    ImGui::TextDisabled("Unhuman Engine (UHE) - Editor");
    ImGui::TextDisabled("Vulkan RHI | Slang | ImGui");
}

void Editor::DrawPreferencesModal()
{
    if (m_ShowPreferences)
    {
        ImGui::OpenPopup("Preferences");
        m_ShowPreferences = false;
    }

    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Preferences", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        DrawPreferencesContent();
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void Editor::DrawAboutModal()
{
    if (m_ShowAbout)
    {
        ImGui::OpenPopup("About UHE");
        m_ShowAbout = false;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("About UHE", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGuiIO& io = ImGui::GetIO();
        ImFont* boldFont = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
        if (boldFont)
            ImGui::PushFont(boldFont);
        ImGui::TextUnformatted("UNHUMAN ENGINE");
        if (boldFont)
            ImGui::PopFont();
        ImGui::TextDisabled("A modern C++ robotics and game engine");
        ImGui::Separator();
        ImGui::TextWrapped("Vulkan RHI, Slang shaders, Jolt physics and an ImGui editor built for "
                           "real-time robotics simulation and synthetic data generation.");
        ImGui::Spacing();
        ImGui::TextDisabled("Theme: %s", EditorTheme::GetName(EditorTheme::GetSelected()));
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void Editor::CreateEntityFromMenu(const char* name, int componentKind)
{
    if (!m_ActiveScene || m_SceneState != SceneState::Edit)
        return;

    Entity entity = m_ActiveScene->CreateEntity(name);
    switch (componentKind)
    {
        case 1: entity.AddComponent<CameraComponent>(); break;
        case 2: entity.AddComponent<SpriteRendererComponent>(); break;
        case 3: entity.AddComponent<TextComponent>(); break;
        case 4: entity.AddComponent<DirectionalLightComponent>(); break;
        case 5: entity.AddComponent<PointLightComponent>(); break;
        case 6: entity.AddComponent<SpotLightComponent>(); break;
        default: break;
    }
    m_SceneHireacyPanel.SetSelectedEntity(entity);
}

void Editor::UI_Toolbar()
{
    f32 padding = 8.0f;
    f32 buttonSize = 24.0f;
    f32 windowSize = buttonSize + padding * 2.0f;

    ImGui::SetNextWindowPos(
        ImVec2(m_ViewportPos.x + (m_ViewportSize.x * 0.5f) - (windowSize * 0.5f), m_ViewportPos.y + 10.0f));
    ImGui::SetNextWindowSize(ImVec2(windowSize, windowSize));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    ImVec4 toolbarBg = EditorTheme::ToolbarBg();
    toolbarBg.w = 0.85f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toolbarBg);

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    auto& colors = ImGui::GetStyle().Colors;
    const auto& buttonHovered = colors[ImGuiCol_ButtonHovered];
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(buttonHovered.x, buttonHovered.y, buttonHovered.z, 0.4f));
    const auto& buttonActive = colors[ImGuiCol_ButtonActive];
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(buttonActive.x, buttonActive.y, buttonActive.z, 0.6f));

    ImGui::Begin("##toolbar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoSavedSettings);

    Ref<Texture2D> icon = m_SceneState == SceneState::Edit ? m_IconPlay : m_IconStop;

    if (ImGui::ImageButton("##PlayStopBtn", (ImTextureID)icon->GetImGuiTextureID(), ImVec2(buttonSize, buttonSize),
                           ImVec2(0, 0), ImVec2(1, 1), ImVec4(0.0f, 0.0f, 0.0f, 0.0f)))
    {
        if (m_SceneState == SceneState::Edit)
            OnScreenPlay();
        else if (m_SceneState == SceneState::Play)
        {
            OnSceneStop();
        }
        else
        {
            UHE_CORE_ERROR("Unknown scene state!");
        }
    }

    ImGui::End();

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(5);
}

} // namespace UHE
