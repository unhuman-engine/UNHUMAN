#include "Editor.h"
#include "EditorTheme.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui.h>
#include "ImGuizmo.h"
#include "UHE/AssestsManager/VfsSystem.h"
#include "UHE/Math/Math.h"
#include "UHE/RHI/RHICommadBuffer.h"
#include "UHE/Renderer3D/Renderer3D.h"

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
    m_IconStop = Texture2D::Create((rootPath / "assets/icon/pause.png").string());
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

    RHI::RenderPassDesc passDesc{};
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

    auto& cmd = device.GetCurrentCommandBuffer();
    cmd.BeginRenderPass(passDesc);
    cmd.SetViewport(0.0f, 0.0f, (float)passDesc.renderWidth, (float)passDesc.renderHeight);
    cmd.SetScissor(0, 0, passDesc.renderWidth, passDesc.renderHeight);

    switch (m_SceneState)
    {
        case SceneState::Edit:
        {
            if (m_ViewPortFocused)
            {
                m_CameraController.OnUpdate(ts);
            }
            m_EditorCamera.OnUpdate(ts);

            m_ActiveScene->OnUpdateEditor(ts, m_EditorCamera);
            break;
        }
        case SceneState::Play:
        {
            m_ActiveScene->OnUpdateRuntime(ts);
            break;
        }
    }

    cmd.EndRenderPass();

    // The swapchain scope belongs to the frame graph now (§14 step 5): the
    // ImGui pass declares it Undefined→ColorAttachment→Present and the
    // executor owns that scope. The legacy empty-swapchain Begin/EndRenderPass
    // here cleared the frame, fought the graph's Load-based scope, and double-
    // transitioned the image to Present before the graph pass ran.
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
    m_HoverdEntity = {};
    m_EditorScene = m_ActiveScene;
    m_ActiveScene = Scene::Copy(m_EditorScene);
    m_ActiveScene->OnRuntimeStart();
    m_SceneState = SceneState::Play;
}

void Editor::OnSceneStop()
{
    m_HoverdEntity = {};
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
        case Key::Q:
            m_GizmoType = 1;
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
    m_ActiveScene = CreateRef<Scene>();
    m_ActiveScene->OnViewportResize((u32)m_ViewPortSize.x, (u32)m_ViewPortSize.y);
    m_SceneHireacyPanel.SetContext(m_ActiveScene);
}

void Editor::OpenScene()
{
    std::string filepath = FileDialogs::OpenFile("UHE Scene (*.unhuman)\0*.unhuman\0");
    if (!filepath.empty())
    {
        m_ActiveScene = CreateRef<Scene>();
        m_ActiveScene->OnViewportResize((u32)m_ViewPortSize.x, (u32)m_ViewPortSize.y);
        m_SceneHireacyPanel.SetContext(m_ActiveScene);

        SceneSerializer serializer(m_ActiveScene);
        serializer.Deserialize(filepath);
    }
}

void Editor::OpenScene(const std::filesystem::path& path)
{
    std::string filepath = path.string();

    if (!filepath.empty())
    {
        m_ActiveScene = CreateRef<Scene>();
        m_ActiveScene->OnViewportResize((u32)m_ViewPortSize.x, (u32)m_ViewPortSize.y);
        m_SceneHireacyPanel.SetContext(m_ActiveScene);
        SceneSerializer serializer(m_ActiveScene);
        serializer.Deserialize(filepath);
    }
}

void DrawConsolePanel()
{
    ImGui::Begin("Console");

    auto sink = UHE::Log::GetImGuiSink();
    const auto& logs = sink->GetLogs();

    for (const auto& log : logs)
    {
        ImVec4 color;

        // Theme-aware console palette (readable on light themes too).
        switch (log.Level)
        {
            case spdlog::level::warn:
                color = EditorTheme::ConsoleWarn();
                break;
            case spdlog::level::err:
                color = EditorTheme::ConsoleError();
                break;
            case spdlog::level::critical:
                color = EditorTheme::ConsoleCritical();
                break;
            default:
                color = EditorTheme::ConsoleInfo();
                break;
        }

        ImGui::TextColored(color, "%s", log.Message.c_str());
    }

    // auto-scroll
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);

    ImGui::End();
}

void Editor::SaveSceneAs()
{
    std::string filepath = FileDialogs::SaveFile("UHE Scene (*.unhuman)\0.unhuman\0");

    if (!filepath.empty())
    {
        SceneSerializer serializer(m_ActiveScene);
        serializer.Serialize(filepath);
    }
}

void Editor::OnImGuiRender()
{
    UHE_PROFILE_FUNCTION();
    EditorTheme::EnsureApplied();

    static bool dockspaceOpen = true;
    static bool opt_fullscreen = true;
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;

    if (opt_fullscreen)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("DockSpace Demo", &dockspaceOpen, window_flags);

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {

            if (ImGui::MenuItem("New Scene", "Ctrl+N"))
            {
                NewScene();
            }

            if (ImGui::MenuItem("Open...", "Ctrl+O"))
            {
                OpenScene();
            }

            if (ImGui::MenuItem("Save As...", "Ctrl+S"))
            {
                SaveSceneAs();
            }

            if (ImGui::MenuItem("Exit"))
                UHE::Application::Get().Close();
            ImGui::EndMenu();
        }
    }
    ImGui::EndMenuBar();

    ImGui::PopStyleVar(2);

    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();
    f32 minWinSizeX = style.WindowMinSize.x;
    style.WindowMinSize.x = 370.0f;
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
    {
        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
    }

    style.WindowMinSize.x = minWinSizeX;

    ImGui::End();

    // your setting
    m_SceneHireacyPanel.OnImGuiRender();
    m_SceneHireacyPanel.ApplyQueuedMutations(); // Issue #17: apply tree edits outside registry iteration
    m_ContentBrowserPanel.OnImGuiRender();
    ImGui::Begin("Settings");

    std::string name = "None";
    if (m_HoverdEntity && m_HoverdEntity.HasComponent<TagComponent>())
        name = m_HoverdEntity.GetComponent<TagComponent>().Tag;

    ImGui::Text("Hover Entity %s", name.c_str());

    ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                ImGui::GetIO().Framerate);

    ImGui::Separator();
    bool enableLighting = Renderer3D::IsLightingEnabled();
    if (ImGui::Checkbox("Enable Basic3D Lighting", &enableLighting)) {
        Renderer3D::SetLightingEnabled(enableLighting);
    }
    
    ImGui::Checkbox("Show Light Icons", &Scene::GetShowLightIcons());

    ImGui::Separator();
    // Issue #17 UX: theme gallery + custom accent picker, persisted.
    if (ImGui::TreeNodeEx("Appearance", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* themeLabel = EditorTheme::GetName(EditorTheme::GetSelected());
        if (ImGui::BeginCombo("Theme", themeLabel))
        {
            int count = EditorTheme::GetThemeCount();
            for (int i = 0; i < count; i++)
            {
                auto id = (EditorTheme::EditorThemeId)i;
                bool selected = (EditorTheme::GetSelected() == id);
                if (ImGui::Selectable(EditorTheme::GetName(id), selected) && !selected)
                    EditorTheme::SetSelected(id);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Custom accent picker: overrides the theme accent everywhere while
        // keeping the theme's backgrounds and text.
        bool customEnabled = EditorTheme::IsCustomAccentEnabled();
        ImVec4 accent = customEnabled ? EditorTheme::GetCustomAccent() : EditorTheme::Accent();
        ImGui::BeginDisabled(!customEnabled);
        ImGuiColorEditFlags accentFlags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
                                          ImGuiColorEditFlags_AlphaPreviewHalf;
        if (ImGui::ColorEdit3("##AccentColor", &accent.x, accentFlags))
            EditorTheme::SetCustomAccent(accent);
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Checkbox("Custom Accent", &customEnabled))
            EditorTheme::SetCustomAccentEnabled(customEnabled);

        if (customEnabled)
        {
            if (ImGui::Button("Reset to Theme Accent"))
                EditorTheme::SetCustomAccentEnabled(false);
        }
        ImGui::TreePop();
    }

    ImGui::End();
    DrawConsolePanel();

    if (!m_IsViewportFullScreen)
        UI_Toolbar();

    ImGuiWindowFlags viewportFlags = 0;
    std::string viewportName = "Viewport";

    if (m_IsViewportFullScreen)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        viewportFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove;
        viewportFlags |= ImGuiWindowFlags_NoDocking;
        viewportName = "FullscreenViewport";
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0, 0});
    ImGui::Begin(viewportName.c_str(), nullptr, viewportFlags);

    if (m_IsViewportFullScreen)
    {
        ImGui::SetWindowFocus();
    }

    auto viewPortOffset = ImGui::GetCursorPos();

    m_ViewportPos = ImGui::GetWindowPos();
    m_ViewportSize = ImGui::GetWindowSize();

    m_ViewPortFocused = ImGui::IsWindowFocused();
    m_ViewPortHover = ImGui::IsWindowHovered();
    Application::Get().GetImguiLayer()->SetBlockEvent(!m_ViewPortFocused && !m_ViewPortHover);

    ImVec2 viewPortSize = ImGui::GetContentRegionAvail();
    m_CameraController.OnResize(viewPortSize.x, viewPortSize.y);
    m_ActiveScene->OnViewportResize(static_cast<u32>(viewPortSize.x), static_cast<u32>(viewPortSize.y));

    m_ViewPortSize = {viewPortSize.x, viewPortSize.y};
    void* textureId = m_Framebuffer->GetColorAttachmentRendererID(0);

    ImGui::Image(textureId, ImVec2{m_ViewPortSize.x, m_ViewPortSize.y}, ImVec2{0, 1}, ImVec2{1, 0});

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

    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 minBound = windowPos;
    minBound.x += viewPortOffset.x;
    minBound.y += viewPortOffset.y;

    ImVec2 maxBound = {minBound.x + viewPortSize.x, minBound.y + viewPortSize.y};
    m_ViewPortBounds[0] = {minBound.x, minBound.y};
    m_ViewPortBounds[1] = {maxBound.x, maxBound.y};

    auto [mx, my] = ImGui::GetMousePos();
    mx -= m_ViewPortBounds[0].x;
    my -= m_ViewPortBounds[0].y;
    glm::vec2 viewportSize = {m_ViewPortBounds[1].x - m_ViewPortBounds[0].x,
                              m_ViewPortBounds[1].y - m_ViewPortBounds[0].y};
    // my = viewportSize.y - my; // Vulkan's Y is usually down, unlike OpenGL where Y is up. Actually wait, did OpenGL
    // invert? Let's invert if OpenGL did it, or just leave it for Vulkan. Let's not invert first. In Vulkan texture Y=0
    // is top.
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
        f32 windowWidth = static_cast<f32>(ImGui::GetWindowWidth());
        f32 windowheight = static_cast<f32>(ImGui::GetWindowHeight());

        ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, windowWidth, windowheight);

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

            bool snap = Input::IsKeyPressed(Key::LeftControl);
            f32 snapValue = 0.5f;

            if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
                snapValue = 45.0f;

            f32 snapValues[3] = {snapValue, snapValue, snapValue};

            ImGuizmo::Manipulate(glm::value_ptr(cameraView), glm::value_ptr(cameraprojection),
                                 (ImGuizmo::OPERATION)m_GizmoType, ImGuizmo::LOCAL, glm::value_ptr(transform), nullptr,
                                 snap ? snapValues : nullptr);

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
