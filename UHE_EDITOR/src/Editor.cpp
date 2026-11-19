#include "Editor.h"
#include "EditorTheme.h"
#include "EditorUI.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include "ImGuizmo.h"
#include "UHE/AssetsManager/AssetImport.h"
#include "UHE/AssetsManager/Project.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Core/CommandLine.h"
#include "UHE/Math/Math.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Scripting/ScriptEngine.h"
#include "UHE/Audio/AudioEngine.h"
#include "UHE/Renderer3D/Renderer3D.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraph.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphExecutor.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

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

    // Shader and script edits show up without a restart while the editor is open;
    // a game build leaves this off, since it ships no sources to watch.
    ShaderCache::Get().SetHotReloadEnabled(true);
    ScriptEngine::Get().SetHotReloadEnabled(true);

    FramebufferSpecification fbSpec;
    fbSpec.Attachments = {FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::RED_INTEGER,
                          FramebufferTextureFormat::Depth};
    fbSpec.Width = 1280;
    fbSpec.Height = 720;
    m_Framebuffer = Framebuffer::Create(fbSpec);

    m_ActiveScene = CreateRef<Scene>();
    m_EditorCamera = EditorCamera(45.0f, 1.766f, 0.1f, 1000.0f);
    m_SceneHireacyPanel.SetContext(m_ActiveScene);

    // A scene loaded before the first ImGui frame resizes the viewport would
    // otherwise create cameras with a 0x0 viewport (a zero aspect asserts in
    // glm::perspective). Seed a sensible size; the first frame refines it.
    if (m_ViewPortSize.x <= 0.0f || m_ViewPortSize.y <= 0.0f)
        m_ViewPortSize = {1280.0f, 720.0f};

    // "Attach to selected entity" in the Content Browser: the panel knows the
    // file, the hierarchy knows the entity, and the editor is where they meet.
    m_ContentBrowserPanel.SetOnAttachScript(
        [this](const std::filesystem::path& script)
        {
            Entity selected = m_SceneHireacyPanel.GetSelectedEntity();
            if (!selected || !m_ActiveScene)
            {
                UHE_CORE_WARN("Attach script: select an entity in the hierarchy first");
                return;
            }

            // Store it the way the engine resolves it: project-relative when the
            // script lives inside the project, so a scene stays portable.
            std::string path = script.string();
            std::error_code ec;
            const std::filesystem::path relative =
                std::filesystem::relative(script, m_ContentBrowserPanel.GetBaseDirectory(), ec);
            if (!ec && !relative.empty() && relative.native().rfind("..", 0) != 0)
            {
                path = relative.generic_string();
            }

            if (selected.HasComponent<ScriptComponent>())
            {
                auto& component = selected.GetComponent<ScriptComponent>();
                component.ScriptPath = path;
                component.RuntimeIndex = 0xFFFFFFFFu; // the new path gets a fresh instance
            }
            else
            {
                selected.AddComponent<ScriptComponent>().ScriptPath = path;
            }

            ScriptEngine::Get().ReloadEntity(*m_ActiveScene, static_cast<u32>(selected));
            UHE_CORE_INFO("Attached script '{0}' to '{1}'", path, selected.GetComponent<TagComponent>().Tag);
            Notify(NotificationCenter::Kind::Success, "Script attached",
                   selected.GetComponent<TagComponent>().Tag + " -> " + path);
        });

    auto rootPath = FileSystem::Get().GetRootPath();
    m_IconPlay = Texture2D::Create((rootPath / "assets/icon/play.png").string());
    m_IconPause = Texture2D::Create((rootPath / "assets/icon/pause.png").string());
    m_IconStop = Texture2D::Create((rootPath / "assets/icon/stop.png").string());

    // An explicit -project wins. Otherwise the most recent project that is still
    // on disk is reopened, so relaunching the editor lands back in the project
    // that was open rather than making the user find it again. `-launcher` asks
    // for the manager instead, and File > Project Manager... returns to it from
    // inside a project.
    const std::string requestedProject = CommandLine::GetValue("-project");
    if (!requestedProject.empty())
    {
        if (!OpenProject(requestedProject))
            m_ProjectLauncher.SetMessage("Could not open '" + requestedProject + "'");
    }
    else if (!CommandLine::HasFlag("-launcher"))
    {
        for (const std::filesystem::path& recent : m_ProjectLauncher.GetRecent())
        {
            // A stale entry is skipped rather than reported: the point of the
            // list is to find a project that is still there.
            if (Project::IsProject(recent) && OpenProject(recent))
                break;
        }
    }
}

void Editor::OnDetach() {}

void Editor::OnUpdate(UHE::Timestep ts)
{
    UHE_PROFILE_FUNCTION();
    m_Notifications.Update(ts.GetSeconds());
    m_SecondsSinceTransformPush += ts.GetSeconds();
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

    // Timestep handed to the renderer. While paused the scene is redrawn from
    // the frozen state (stepping advances it by exactly one frame), so sprite and
    // model animations must not keep ticking on the render path.
    f32 renderTs = ts;

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
                    m_ActiveScene->OnUpdateRuntime(ts); // simulation + render
                m_StepRequested = false;
            }
            else
            {
                renderTs = 0.0f;
                // The graph path renders the runtime scene in EndFrameGraph; the
                // immediate path owns the render pass here and has to draw, or
                // pausing would leave the viewport black.
                if (!graphMode)
                    m_ActiveScene->RenderRuntimeScene(renderTs);
            }
            break;
        }
    }

    if (graphMode)
    {
        // Scene rendering is deferred into EndFrameGraph so the compiler owns
        // the attachment transitions; the ImGui pass is declared after this
        // and therefore runs second (UI on top).
        DeclareSceneGraphPass(renderTs, runtimeScene);
    }
    else
    {
        cmd.EndRenderPass();
    }

    // The editor camera is the listener while editing; playing uses the scene's
    // primary camera so 3D audio follows what the player sees.
    glm::vec3 listenerPosition = m_EditorCamera.GetPosition();
    glm::vec3 listenerForward = m_EditorCamera.GetForwardDirection();
    glm::vec3 listenerUp = m_EditorCamera.GetUpDirection();

    if (runtimeScene && m_ActiveScene)
    {
        Entity cameraEntity = m_ActiveScene->GetPrimaryCameraEntity();
        if (cameraEntity && cameraEntity.HasComponent<TransformComponent>())
        {
            const glm::mat4 world = m_ActiveScene->GetWorldSpaceTransformMatrix(cameraEntity);
            listenerPosition = glm::vec3(world[3]);
            const glm::vec3 forward = -glm::vec3(world[2]);
            const glm::vec3 up = glm::vec3(world[1]);
            if (glm::length(forward) > 0.0001f)
                listenerForward = glm::normalize(forward);
            if (glm::length(up) > 0.0001f)
                listenerUp = glm::normalize(up);
        }
    }

    Audio::AudioEngine::SetListenerPosition(listenerPosition, listenerForward, listenerUp);

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
    dispatcher.Dispatch<WindowFileDropEvent>(UHE_BIND_EVENT_FN(Editor::OnFileDrop));
}

bool Editor::OnFileDrop(WindowFileDropEvent& e)
{
    if (!m_Project)
    {
        m_ProjectLauncher.SetMessage("Open a project before importing files");
        return false;
    }

    // Everything the user drops is copied into the project first, so a scene can
    // never reference a file that lives outside it.
    const std::vector<std::filesystem::path> imported = m_ContentBrowserPanel.ImportFiles(e.GetPaths());
    if (imported.empty())
        return false;

    const glm::vec2& viewportMin = m_ViewPortBounds[0];
    const glm::vec2& viewportMax = m_ViewPortBounds[1];
    const bool overViewport = e.GetX() >= viewportMin.x && e.GetX() <= viewportMax.x &&
                              e.GetY() >= viewportMin.y && e.GetY() <= viewportMax.y;

    for (const std::filesystem::path& file : imported)
    {
        if (!overViewport)
            continue; // the Content Browser already shows the imported file
        CreateEntityFromAsset(file);
    }

    if (!overViewport)
    {
        if (imported.size() == 1)
            Notify(NotificationCenter::Kind::Success, "Imported file", imported.front().filename().string());
        else
            Notify(NotificationCenter::Kind::Success, "Imported files",
                   std::to_string(imported.size()) + " files added to the project");
    }
    return true;
}

bool Editor::CreateEntityFromAsset(const std::filesystem::path& assetPath)
{
    if (!m_ActiveScene || assetPath.empty())
        return false;

    // Components store project-relative paths so the scene stays portable.
    const std::string storedPath = m_Project ? MakeAssetRelativePath(assetPath, m_Project->AssetPath)
                                             : assetPath.generic_string();
    const std::string extension = assetPath.extension().string();
    const std::string name = assetPath.stem().string();

    if (extension == ".unhuman")
    {
        OpenScene(assetPath);
        return true;
    }

    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
        extension == ".tga" || extension == ".hdr" || extension == ".ktx2")
    {
        Entity sprite = m_ActiveScene->CreateEntity(name);
        auto& src = sprite.AddComponent<SpriteRendererComponent>();
        src.TexturePath = storedPath;
        src.Texture = Texture2D::Create(storedPath);
        if (src.Texture)
        {
            UHE_CORE_INFO("Imported texture '{0}' into the scene", storedPath);
            Notify(NotificationCenter::Kind::Success, "Added texture", name);
        }
        else
        {
            UHE_CORE_ERROR("Could not load imported texture '{0}'", storedPath);
            Notify(NotificationCenter::Kind::Error, "Could not load texture", storedPath);
        }
        m_SceneHireacyPanel.SetSelectedEntity(sprite);
        return true;
    }

    if (extension == ".wav" || extension == ".ogg")
    {
        Entity audioEntity = m_ActiveScene->CreateEntity(name);
        auto& audio = audioEntity.AddComponent<AudioSourceComponent>();
        audio.SoundPath = storedPath;
        m_SceneHireacyPanel.SetSelectedEntity(audioEntity);
        Notify(NotificationCenter::Kind::Success, "Added audio source", name);
        return true;
    }

    if (extension == ".gltf" || extension == ".glb" || extension == ".fbx" || extension == ".obj")
    {
        Entity model = m_ActiveScene->CreateEntity(name);
        auto& mc = model.AddComponent<Model3DComponent>();
        mc.ModelPath = storedPath;
        mc.IsLoaded = mc.ModelData && mc.ModelData->loadModel(storedPath);
        if (mc.IsLoaded)
        {
            UHE_CORE_INFO("Imported model '{0}' into the scene", storedPath);
            Notify(NotificationCenter::Kind::Success, "Added model", name);
        }
        else
        {
            UHE_CORE_ERROR("Could not load imported model '{0}'", storedPath);
            Notify(NotificationCenter::Kind::Error, "Could not load model", storedPath);
        }
        m_SceneHireacyPanel.SetSelectedEntity(model);
        return true;
    }

    UHE_CORE_INFO("Imported '{0}' into the project (no scene entity for this type)", storedPath);
    Notify(NotificationCenter::Kind::Info, "Imported file", name);
    return false;
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
        m_UndoStack.clear();
        m_RedoStack.clear();
        ResetTransformBaseline();
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
    m_UndoStack.clear();
    m_RedoStack.clear();
    ResetTransformBaseline();
}

void Editor::Notify(NotificationCenter::Kind kind, std::string title, std::string message)
{
    m_Notifications.Push(kind, std::move(title), std::move(message));
}

void Editor::FocusSelectedEntity()
{
    if (!m_ActiveScene)
        return;

    Entity selected = m_SceneHireacyPanel.GetSelectedEntity();
    if (!selected || !selected.HasComponent<TransformComponent>())
        return;

    const glm::mat4 world = m_ActiveScene->GetWorldSpaceTransformMatrix(selected);
    const glm::vec3 position(world[3]);
    const TransformComponent& transform = selected.GetComponent<TransformComponent>();
    const float extent = std::max({std::abs(transform.Scale.x), std::abs(transform.Scale.y),
                                   std::abs(transform.Scale.z)});

    m_EditorCamera.FocusOn(position, glm::clamp(4.0f * extent, 1.5f, 200.0f));
}

void Editor::DeleteSelectedEntity()
{
    if (!m_ActiveScene || m_SceneState != SceneState::Edit)
        return;

    Entity selected = m_SceneHireacyPanel.GetSelectedEntity();
    if (!selected)
        return;

    std::string name = "entity";
    if (selected.HasComponent<TagComponent>())
        name = selected.GetComponent<TagComponent>().Tag;

    m_ActiveScene->DestroyEntity(selected);
    m_SceneHireacyPanel.SetSelectedEntity(Entity());
    m_HoverdEntity = Entity();
    ResetTransformBaseline();
    Notify(NotificationCenter::Kind::Info, "Deleted '" + name + "'", "Ctrl+Z restores transforms only.");
}

void Editor::ReloadShaders()
{
    ShaderCache::Get().ReloadAll();
    Notify(NotificationCenter::Kind::Success, "Shaders reloaded",
           "Every loaded shader was recompiled and its pipelines rebuilt.");
}

void Editor::ReloadScripts()
{
    if (!m_ActiveScene)
    {
        Notify(NotificationCenter::Kind::Warning, "No scene to reload");
        return;
    }

    ScriptEngine::Get().ReloadAll(*m_ActiveScene);

    int total = 0;
    int errored = 0;
    for (const ScriptEngine::InstanceInfo& info : ScriptEngine::Get().Instances(*m_ActiveScene))
    {
        total++;
        if (info.Errored)
            errored++;
    }

    if (errored > 0)
        Notify(NotificationCenter::Kind::Warning, "Scripts reloaded with errors",
               std::to_string(errored) + " of " + std::to_string(total) + " instances failed - see Scripts.");
    else
        Notify(NotificationCenter::Kind::Success, "Scripts reloaded",
               std::to_string(total) + " instance(s) recreated.");
}

void Editor::ResetTransformBaseline()
{
    m_TransformBaselineEntity = 0;
    m_SecondsSinceTransformPush = 1000.0f;
}

void Editor::PollTransformUndo()
{
    if (!m_ActiveScene || m_SceneState != SceneState::Edit)
    {
        ResetTransformBaseline();
        return;
    }

    Entity selected = m_SceneHireacyPanel.GetSelectedEntity();
    if (!selected || !selected.HasComponent<TransformComponent>())
    {
        ResetTransformBaseline();
        return;
    }

    const u64 entityID = selected.GetUUID();
    const TransformComponent& transform = selected.GetComponent<TransformComponent>();
    const TransformSnapshot current{entityID, transform.Translation, transform.Rotation, transform.Scale};

    if (entityID != m_TransformBaselineEntity)
    {
        // A different entity became selected: its current transform is the
        // state a later edit should return to, not a new history entry.
        m_TransformBaseline = current;
        m_TransformBaselineEntity = entityID;
        m_SecondsSinceTransformPush = 1000.0f;
        return;
    }

    const bool changed = m_TransformBaseline.Translation != current.Translation ||
                         m_TransformBaseline.Rotation != current.Rotation ||
                         m_TransformBaseline.Scale != current.Scale;
    if (!changed)
        return;

    // A drag is still in progress: keep the pre-drag baseline so the release
    // produces exactly one step.
    if (ImGui::IsAnyMouseDown() || ImGuizmo::IsUsing())
        return;

    // Typing in a transform field produces a burst of small changes; within a
    // second of the previous push they collapse into the same history entry.
    if (m_SecondsSinceTransformPush > 1.0f)
    {
        m_UndoStack.push_back(m_TransformBaseline);
        if (m_UndoStack.size() > 200)
            m_UndoStack.erase(m_UndoStack.begin());
        m_RedoStack.clear();
        m_SecondsSinceTransformPush = 0.0f;
    }

    m_TransformBaseline = current;
}

void Editor::UndoTransform()
{
    if (!m_ActiveScene || m_UndoStack.empty())
    {
        Notify(NotificationCenter::Kind::Info, "Nothing to undo");
        return;
    }

    while (!m_UndoStack.empty())
    {
        const TransformSnapshot target = m_UndoStack.back();
        m_UndoStack.pop_back();

        Entity entity = m_ActiveScene->GetEntityWithUUID(target.EntityID);
        if (!entity || !entity.HasComponent<TransformComponent>())
            continue; // the entity was deleted after the edit

        TransformComponent& transform = entity.GetComponent<TransformComponent>();
        m_RedoStack.push_back({target.EntityID, transform.Translation, transform.Rotation, transform.Scale});
        transform.Translation = target.Translation;
        transform.Rotation = target.Rotation;
        transform.Scale = target.Scale;

        m_SceneHireacyPanel.SetSelectedEntity(entity);
        m_TransformBaseline = target;
        m_TransformBaselineEntity = target.EntityID;
        m_SecondsSinceTransformPush = 1000.0f;
        return;
    }

    m_RedoStack.clear();
    Notify(NotificationCenter::Kind::Info, "Nothing to undo");
}

void Editor::RedoTransform()
{
    if (!m_ActiveScene || m_RedoStack.empty())
    {
        Notify(NotificationCenter::Kind::Info, "Nothing to redo");
        return;
    }

    while (!m_RedoStack.empty())
    {
        const TransformSnapshot target = m_RedoStack.back();
        m_RedoStack.pop_back();

        Entity entity = m_ActiveScene->GetEntityWithUUID(target.EntityID);
        if (!entity || !entity.HasComponent<TransformComponent>())
            continue;

        TransformComponent& transform = entity.GetComponent<TransformComponent>();
        m_UndoStack.push_back({target.EntityID, transform.Translation, transform.Rotation, transform.Scale});
        transform.Translation = target.Translation;
        transform.Rotation = target.Rotation;
        transform.Scale = target.Scale;

        m_SceneHireacyPanel.SetSelectedEntity(entity);
        m_TransformBaseline = target;
        m_TransformBaselineEntity = target.EntityID;
        m_SecondsSinceTransformPush = 1000.0f;
        return;
    }

    m_UndoStack.clear();
    Notify(NotificationCenter::Kind::Info, "Nothing to redo");
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
            else if (!controlPressed && !shift)
            {
                FocusSelectedEntity();
            }
            break;
        }
        case Key::P:
        {
            if (controlPressed && m_Project)
                m_CommandPalette.Toggle();
            break;
        }
        case Key::Z:
        {
            if (controlPressed)
            {
                if (shift)
                    RedoTransform();
                else
                    UndoTransform();
            }
            break;
        }
        case Key::Y:
        {
            if (controlPressed)
                RedoTransform();
            break;
        }
        case Key::Delete:
        {
            if (!controlPressed && !shift)
                DeleteSelectedEntity();
            break;
        }
        case Key::Escape:
        {
            if (m_CommandPalette.IsOpen())
                m_CommandPalette.Close();
            else if (m_IsViewportFullScreen)
                m_IsViewportFullScreen = false;
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

void Editor::NewScene(bool notify)
{
    OnSceneStop();
    m_ActiveScene = CreateRef<Scene>();
    m_ActiveScene->OnViewportResize((u32)m_ViewPortSize.x, (u32)m_ViewPortSize.y);
    m_SceneHireacyPanel.SetContext(m_ActiveScene);
    m_CurrentScenePath.clear();
    m_UndoStack.clear();
    m_RedoStack.clear();
    ResetTransformBaseline();
    if (notify)
        Notify(NotificationCenter::Kind::Info, "New scene", "Unsaved changes in the previous scene were dropped.");
}

void Editor::OpenScene()
{
    // Scenes live inside the project; start the dialog there.
    std::string filepath = FileDialogs::OpenFile("UHE Scene (*.unhuman)\0*.unhuman\0", ProjectAssetPath().string());
    if (!filepath.empty())
    {
        OpenScene(filepath);
    }
}

void Editor::OpenScene(const std::filesystem::path& path, bool notify)
{
    std::string filepath = path.string();

    if (!filepath.empty())
    {
        // Load into a fresh scene and switch only on success: switching first
        // left a failed open showing an empty scene bound to this path, which
        // the next save would have written over the user's file.
        Ref<Scene> loaded = CreateRef<Scene>();
        SceneSerializer serializer(loaded);
        if (!serializer.Deserialize(filepath))
        {
            Notify(NotificationCenter::Kind::Error, "Could not open scene",
                   path.filename().string() + " (see the console for why)");
            return;
        }

        OnSceneStop();
        m_ActiveScene = loaded;
        m_ActiveScene->OnViewportResize((u32)m_ViewPortSize.x, (u32)m_ViewPortSize.y);
        m_SceneHireacyPanel.SetContext(m_ActiveScene);
        m_CurrentScenePath = path;
        m_UndoStack.clear();
        m_RedoStack.clear();
        ResetTransformBaseline();
        if (notify)
            Notify(NotificationCenter::Kind::Info, "Scene opened", path.filename().string());
    }
}

bool Editor::OpenProject(const std::filesystem::path& path)
{
    ProjectInfo info;
    std::string error;
    if (!Project::Load(path, info, &error))
    {
        UHE_CORE_ERROR("Could not open project '{0}': {1}", path.string(), error);
        Notify(NotificationCenter::Kind::Error, "Could not open project", error.empty() ? path.string() : error);
        return false;
    }

    SetProject(std::move(info));
    return true;
}

void Editor::SetProject(ProjectInfo info)
{
    std::string error;
    if (!Project::EnsureAssetFolder(info, &error))
        UHE_CORE_WARN("Project '{0}': {1}", info.Name, error);

    // "game:" -> the project's Assets folder, "engine:" -> the folder shipped
    // next to the binary. Mounting the project last keeps it ahead of the engine
    // assets, so a project can shadow an engine file by name.
    Project::Mount(info, FileSystem::Get().GetRootPath() / "assets");

    m_Project = std::move(info);
    m_ShowLauncher = false;
    m_ProjectLauncher.AddRecent(*m_Project);
    m_ContentBrowserPanel.SetBaseDirectory(m_Project->AssetPath);

    const std::filesystem::path startScene = Project::StartScenePath(*m_Project);
    std::error_code ec;
    if (!startScene.empty() && std::filesystem::is_regular_file(startScene, ec))
        OpenScene(startScene, false);
    else
        NewScene(false);

    UHE_CORE_INFO("Opened project '{0}' ({1})", m_Project->Name, m_Project->RootPath.string());
    Notify(NotificationCenter::Kind::Success, "Project opened", m_Project->Name);
}

void Editor::CloseProject()
{
    m_Project.reset();
    m_ShowLauncher = true;
    m_ProjectLauncher.SetMessage("");
    m_UndoStack.clear();
    m_RedoStack.clear();
    ResetTransformBaseline();
    m_CommandPalette.Close();
    Notify(NotificationCenter::Kind::Info, "Project closed");
}

std::filesystem::path Editor::ProjectAssetPath() const
{
    return m_Project ? m_Project->AssetPath : std::filesystem::path();
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
    std::string filepath = FileDialogs::SaveFile("UHE Scene (*.unhuman)\0.unhuman\0", ProjectAssetPath().string());

    if (!filepath.empty())
    {
        SceneSerializer serializer(m_ActiveScene);
        serializer.Serialize(filepath);
        m_CurrentScenePath = filepath;
        Notify(NotificationCenter::Kind::Success, "Scene saved",
               std::filesystem::path(filepath).filename().string());
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
        ImGui::DockBuilderDockWindow("Scripts", bottomRight);
        ImGui::DockBuilderDockWindow("Audio", bottomRight);
        // The graph needs canvas space: share the central node with the
        // viewport rather than squeezing into a bottom tab.
        ImGui::DockBuilderDockWindow("Animation Graph", centerTop);
        ImGui::DockBuilderFinish(dockspaceId);
    }
} // namespace

void Editor::OnImGuiRender()
{
    UHE_PROFILE_FUNCTION();
    EditorTheme::EnsureApplied();

    // No project yet: the launcher owns the screen, exactly like a real engine
    // editor. Nothing else is submitted, so no panel can touch a missing asset root.
    if (m_ShowLauncher && !m_Project)
    {
        ProjectInfo opened;
        if (m_ProjectLauncher.OnImGuiRender(opened))
            SetProject(std::move(opened));
        else
        {
            m_CommandPalette.Close();
            m_Notifications.Draw();
        }
        return;
    }

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

    // v3: adds Animation Graph and Audio to the default docking layout. A new
    // id makes existing ini layouts rebuild once instead of leaving the new
    // panels floating.
    const ImGuiID dockspaceId = ImGui::GetID("UHE_DockSpace_v3");
    if (m_ResetLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
    {
        BuildDefaultLayout(dockspaceId, ImGui::GetContentRegionAvail());
        m_ResetLayout = false;
    }

    // The dockspace must leave room for the status bar *and* the item spacing
    // ImGui inserts before it. Reserving only the bar leaves the host window's
    // content ~6px taller than its inner rect, which makes the whole dockspace
    // scrollable by those few pixels (a full-height scrollbar that barely moves).
    const f32 statusBarHeight = ImGui::GetFrameHeight() + 6.0f;
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, -(statusBarHeight + style.ItemSpacing.y)),
                     ImGuiDockNodeFlags_None);
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

    m_ScriptsPanel.SetContext(m_ActiveScene.get());
    if (m_ShowScripts)
        m_ScriptsPanel.OnImGuiRender(m_ShowScripts);

    if (m_ShowAnimationGraph)
        m_AnimationGraphPanel.OnImGuiRender(m_ShowAnimationGraph, m_SceneHireacyPanel.GetSelectedEntity());

    if (m_ShowAudio)
        m_AudioPanel.OnImGuiRender(m_ShowAudio);

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
    Application::Get().GetImguiLayer()->SetBlockEvent(m_CommandPalette.IsOpen() ||
                                                      (!m_ViewPortFocused && !m_ViewPortHover));

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
            // Shared with OS drops: stores a project-relative path and builds the
            // matching entity (texture, model, or opens a scene).
            CreateEntityFromAsset(std::filesystem::path(path));
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
            f32 snapValue = m_SnapValue;

            if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
                snapValue = 15.0f;

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

    if (m_ShowViewportOverlay)
        DrawViewportOverlay(imageMin, imageMax);
    }

    // Record the finished edit after the gizmo and inspector have run, then
    // draw the editor-level overlays on top of every docked panel.
    PollTransformUndo();
    m_Notifications.Draw();
    m_CommandPalette.OnImGuiRender(BuildCommands());
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

void Editor::DrawViewportOverlay(const ImVec2& imageMin, const ImVec2& imageMax)
{
    const float width = imageMax.x - imageMin.x;
    const float height = imageMax.y - imageMin.y;
    if (width < 140.0f || height < 90.0f)
        return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();

    // Small translucent pill: coloured dot + label. Kept on black so it reads
    // on both the light and the dark themes.
    float badgeX = imageMin.x + 10.0f;
    const float badgeY = imageMin.y + 10.0f;
    auto drawBadge = [&](const char* text, const ImVec4& accent)
    {
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const ImVec2 badgeMin(badgeX, badgeY);
        const ImVec2 badgeMax(badgeX + textSize.x + 20.0f, badgeY + textSize.y + 7.0f);
        const float rounding = (badgeMax.y - badgeMin.y) * 0.5f;

        draw->AddRectFilled(badgeMin, badgeMax, IM_COL32(12, 12, 16, 165), rounding);
        draw->AddRect(badgeMin, badgeMax, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.55f)),
                      rounding);
        draw->AddCircleFilled(ImVec2(badgeMin.x + 9.0f, (badgeMin.y + badgeMax.y) * 0.5f), 3.0f,
                              ImGui::GetColorU32(accent));
        draw->AddText(ImVec2(badgeMin.x + 15.0f, badgeMin.y + 3.5f), IM_COL32(238, 238, 242, 255), text);

        badgeX = badgeMax.x + 6.0f;
    };

    const char* toolName = "Select";
    ImVec4 toolAccent = EditorTheme::Accent();
    switch (m_GizmoType)
    {
        case ImGuizmo::TRANSLATE:
            toolName = "Move";
            break;
        case ImGuizmo::ROTATE:
            toolName = "Rotate";
            break;
        case ImGuizmo::SCALE:
            toolName = "Scale";
            break;
        default:
            break;
    }
    drawBadge(toolName, toolAccent);

    if (m_GizmoType != -1)
    {
        char space[32];
        std::snprintf(space, sizeof(space), "%s Space", m_GizmoSpaceWorld ? "World" : "Local");
        drawBadge(space, EditorTheme::ConsoleInfo());

        if (m_SnapEnabled)
        {
            char snap[32];
            std::snprintf(snap, sizeof(snap), "Snap %.2f", m_SnapValue);
            drawBadge(snap, EditorTheme::Warning());
        }
    }

    drawBadge(m_RenderPath == RenderPath::RenderGraph ? "Render Graph" : "Immediate", EditorTheme::Success());

    // Playing without a camera renders nothing: say so instead of showing a
    // black viewport.
    if (m_SceneState != SceneState::Edit && m_ActiveScene && !m_ActiveScene->GetPrimaryCameraEntity())
    {
        const char* warning = "No primary camera - the runtime view is blank";
        const ImVec2 textSize = ImGui::CalcTextSize(warning);
        const ImVec2 warnMin(imageMin.x + (width - textSize.x) * 0.5f - 12.0f, imageMin.y + 12.0f);
        const ImVec2 warnMax(warnMin.x + textSize.x + 24.0f, warnMin.y + textSize.y + 10.0f);
        draw->AddRectFilled(warnMin, warnMax, IM_COL32(60, 20, 18, 220), 6.0f);
        draw->AddRect(warnMin, warnMax, ImGui::GetColorU32(EditorTheme::Danger()), 6.0f);
        draw->AddText(ImVec2(warnMin.x + 12.0f, warnMin.y + 5.0f),
                      ImGui::GetColorU32(EditorTheme::Warning()), warning);
    }

    DrawAxisGizmo(draw, ImVec2(imageMin.x + 48.0f, imageMax.y - 48.0f), 30.0f);

    // Interactive camera controls, anchored to the top-right corner. Submitted
    // last so it sits above the docked viewport window.
    ImGui::SetNextWindowPos(ImVec2(imageMax.x - 10.0f, imageMin.y + 8.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.82f);
    const ImGuiWindowFlags controlFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoFocusOnAppearing |
                                          ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::Begin("##ViewportControls", nullptr, controlFlags);
    ImGui::PopStyleVar(2);

    ImGui::TextDisabled("Camera");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    float speed = m_EditorCamera.GetSpeedMultiplier();
    if (ImGui::SliderFloat("##CameraSpeed", &speed, 0.1f, 5.0f, "%.2fx"))
        m_EditorCamera.SetSpeedMultiplier(speed);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pan / orbit / zoom sensitivity");
    ImGui::SameLine();
    if (ImGui::Button("Focus"))
        FocusSelectedEntity();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Frame the selected entity (F)");

    ImGui::End();
}

void Editor::DrawAxisGizmo(ImDrawList* draw, const ImVec2& center, float radius)
{
    draw->AddCircleFilled(center, radius + 7.0f, IM_COL32(12, 12, 16, 120), 32);
    draw->AddCircle(center, radius + 7.0f, ImGui::GetColorU32(ImGuiCol_Separator), 32, 1.0f);

    const glm::mat3 viewRotation(m_EditorCamera.GetViewMatrix());
    struct Axis
    {
        glm::vec3 Direction;
        ImVec4 Color;
        const char* Label;
    };
    const Axis axes[3] = {
        {{1.0f, 0.0f, 0.0f}, {0.96f, 0.34f, 0.32f, 1.0f}, "X"},
        {{0.0f, 1.0f, 0.0f}, {0.48f, 0.88f, 0.38f, 1.0f}, "Y"},
        {{0.0f, 0.0f, 1.0f}, {0.38f, 0.64f, 0.98f, 1.0f}, "Z"},
    };

    ImVec2 endpoints[3];
    float depths[3];
    for (int i = 0; i < 3; ++i)
    {
        const glm::vec3 viewDirection = viewRotation * axes[i].Direction;
        const float length = radius * (0.62f + 0.38f * std::abs(viewDirection.z));
        endpoints[i] = ImVec2(center.x + viewDirection.x * length, center.y - viewDirection.y * length);
        depths[i] = viewDirection.z;
    }

    // Draw the axis pointing away from the camera first, so the one pointing
    // at the viewer stays readable on top.
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int a, int b) { return depths[a] < depths[b]; });

    for (int axisIndex : order)
    {
        const Axis& axis = axes[axisIndex];
        draw->AddLine(center, endpoints[axisIndex], ImGui::GetColorU32(axis.Color), 2.0f);
        draw->AddCircleFilled(endpoints[axisIndex], 4.0f, ImGui::GetColorU32(axis.Color), 12);

        const ImVec2 labelOffset(endpoints[axisIndex].x - center.x, endpoints[axisIndex].y - center.y);
        const float labelLength = std::max(std::sqrt(labelOffset.x * labelOffset.x + labelOffset.y * labelOffset.y),
                                           0.001f);
        const ImVec2 labelPosition(endpoints[axisIndex].x + labelOffset.x / labelLength * 8.0f - 3.0f,
                                   endpoints[axisIndex].y + labelOffset.y / labelLength * 8.0f - 7.0f);
        draw->AddText(labelPosition, ImGui::GetColorU32(axis.Color), axis.Label);
    }
}

std::vector<CommandPalette::Command> Editor::BuildCommands()
{
    std::vector<CommandPalette::Command> commands;
    commands.reserve(48);

    auto add = [&commands](std::string label, std::string category, std::string shortcut, bool enabled,
                           std::function<void()> action)
    {
        commands.push_back({std::move(label), std::move(category), std::move(shortcut), enabled,
                            std::move(action)});
    };

    add("New Scene", "File", "Ctrl+N", true, [this] { NewScene(); });
    add("Open Scene...", "File", "Ctrl+O", true, [this] { OpenScene(); });
    add("Save Scene As...", "File", "Ctrl+S", true, [this] { SaveSceneAs(); });
    add("Open Project...", "File", "", true,
        [this]
        {
            const std::string folder = FileDialogs::SelectFolder("Open Project",
                                                                 std::filesystem::current_path().string());
            if (!folder.empty() && !OpenProject(folder))
                m_ProjectLauncher.SetMessage("Could not open '" + folder + "'");
        });
    add("Project Manager...", "File", "", m_Project.has_value(), [this] { CloseProject(); });

    add("Undo Transform", "Edit", "Ctrl+Z", !m_UndoStack.empty(), [this] { UndoTransform(); });
    add("Redo Transform", "Edit", "Ctrl+Shift+Z", !m_RedoStack.empty(), [this] { RedoTransform(); });
    add("Delete Selected Entity", "Edit", "Del", m_SceneState == SceneState::Edit,
        [this] { DeleteSelectedEntity(); });
    add("Preferences...", "Edit", "Ctrl+,", true, [this] { m_ShowPreferences = true; });
    add("About UHE", "Help", "F1", true, [this] { m_ShowAbout = true; });

    add("Toggle Hierarchy", "View", "", true, [this] { m_ShowHierarchy = !m_ShowHierarchy; });
    add("Toggle Inspector", "View", "", true, [this] { m_ShowInspector = !m_ShowInspector; });
    add("Toggle Content Browser", "View", "", true, [this] { m_ShowContentBrowser = !m_ShowContentBrowser; });
    add("Toggle Console", "View", "", true, [this] { m_ShowConsole = !m_ShowConsole; });
    add("Toggle Profiler", "View", "", true, [this] { m_ShowProfiler = !m_ShowProfiler; });
    add("Toggle Scripts", "View", "", true, [this] { m_ShowScripts = !m_ShowScripts; });
    add("Toggle Animation Graph", "View", "", true, [this] { m_ShowAnimationGraph = !m_ShowAnimationGraph; });
    add("Toggle Audio", "View", "", true, [this] { m_ShowAudio = !m_ShowAudio; });
    add("Toggle Settings", "View", "", true, [this] { m_ShowSettings = !m_ShowSettings; });
    add("Toggle Viewport", "View", "", true, [this] { m_ShowViewport = !m_ShowViewport; });
    add("Toggle Viewport Overlay", "View", "", true, [this] { m_ShowViewportOverlay = !m_ShowViewportOverlay; });
    add("Toggle Fullscreen Viewport", "View", "Ctrl+Shift+F", true,
        [this] { m_IsViewportFullScreen = !m_IsViewportFullScreen; });
    add("Reset Layout", "View", "", true, [this] { m_ResetLayout = true; });
    add("Focus Selected Entity", "View", "F", true, [this] { FocusSelectedEntity(); });

    add("Play", "Play", "F5", m_SceneState == SceneState::Edit, [this] { OnScreenPlay(); });
    add("Pause / Resume", "Play", "F6", m_SceneState != SceneState::Edit, [this] { OnScreenPause(); });
    add("Step One Frame", "Play", "F7", m_SceneState == SceneState::Paused, [this] { OnScreenStep(); });
    add("Stop", "Play", "F5", m_SceneState != SceneState::Edit, [this] { OnSceneStop(); });

    add("Use Immediate Render Path", "Render", "", m_RenderPath != RenderPath::Immediate,
        [this] { m_RenderPath = RenderPath::Immediate; });
    add("Use Render Graph Path", "Render", "", m_RenderPath != RenderPath::RenderGraph,
        [this] { m_RenderPath = RenderPath::RenderGraph; });
    add("Toggle Lighting", "Render", "", true,
        [this] { Renderer3D::SetLightingEnabled(!Renderer3D::IsLightingEnabled()); });
    add("Reload Shaders", "Render", "Ctrl+Shift+R", true, [this] { ReloadShaders(); });
    add("Reload Scripts", "Render", "Ctrl+Alt+R", true, [this] { ReloadScripts(); });

    for (int i = 0; i < EditorTheme::GetThemeCount(); ++i)
    {
        const auto theme = static_cast<EditorTheme::EditorThemeId>(i);
        const bool selected = EditorTheme::GetSelected() == theme;
        add(std::string("Theme: ") + EditorTheme::GetName(theme), "Appearance", selected ? "current" : "",
            !selected, [theme] { EditorTheme::SetSelected(theme); });
    }

    // Scene entities are listed as a convenience: picking one selects it and
    // frames it, which is faster than hunting through the hierarchy.
    if (m_ActiveScene && m_ActiveScene->GetRegistry().view<entt::entity>().size() <= 200)
    {
        for (entt::entity handle : m_ActiveScene->GetRegistry().view<entt::entity>())
        {
            Entity entity(handle, m_ActiveScene.get());
            if (!entity || !entity.HasComponent<TagComponent>())
                continue;
            const std::string tag = entity.GetComponent<TagComponent>().Tag;
            add("Select: " + tag, "Scene", "", true,
                [this, entity]()
                {
                    m_SceneHireacyPanel.SetSelectedEntity(entity);
                    FocusSelectedEntity();
                });
        }
    }

    return commands;
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
        if (ImGui::MenuItem("Open Project..."))
        {
            const std::string folder = FileDialogs::SelectFolder("Open Project", std::filesystem::current_path().string());
            if (!folder.empty() && !OpenProject(folder))
                m_ProjectLauncher.SetMessage("Could not open '" + folder + "'");
        }
        if (ImGui::MenuItem("Project Manager..."))
            CloseProject();
        ImGui::Separator();
        if (ImGui::MenuItem("Reload Shaders", "Ctrl+Shift+R"))
        {
            // Compiles every loaded shader from source and rebuilds the pipelines
            // built from it, without restarting.
            ReloadShaders();
        }
        if (ImGui::MenuItem("Reload Scripts", "Ctrl+Alt+R"))
        {
            // Recompiles every attached script and runs its OnCreate again, for
            // the cases a file timestamp cannot see.
            ReloadScripts();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4"))
            Application::Get().Close();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        ImGui::BeginDisabled(m_UndoStack.empty());
        if (ImGui::MenuItem("Undo Transform", "Ctrl+Z"))
            UndoTransform();
        ImGui::EndDisabled();
        ImGui::BeginDisabled(m_RedoStack.empty());
        if (ImGui::MenuItem("Redo Transform", "Ctrl+Shift+Z"))
            RedoTransform();
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Command Palette...", "Ctrl+P"))
            m_CommandPalette.Open();
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
        EditorUI::ToggleMenuItem("Scripts", &m_ShowScripts);
        EditorUI::ToggleMenuItem("Animation Graph", &m_ShowAnimationGraph);
        EditorUI::ToggleMenuItem("Audio", &m_ShowAudio);
        EditorUI::ToggleMenuItem("Settings", &m_ShowSettings);
        EditorUI::ToggleMenuItem("Viewport", &m_ShowViewport);
        ImGui::Separator();
        EditorUI::ToggleMenuItem("Viewport Overlay", &m_ShowViewportOverlay, "Gizmo and camera HUD");
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
    ImGui::SameLine(0.0f, 4.0f);
    if (EditorUI::ToolbarToggle("Commands", m_CommandPalette.IsOpen(), "Command palette (Ctrl+P)", ImVec2(88, 28)))
        m_CommandPalette.Open();
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::BeginDisabled(m_UndoStack.empty());
    if (ImGui::Button("Undo", ImVec2(52.0f, 28.0f)))
        UndoTransform();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Undo the last transform edit (Ctrl+Z)");
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::BeginDisabled(m_RedoStack.empty());
    if (ImGui::Button("Redo", ImVec2(52.0f, 28.0f)))
        RedoTransform();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Redo the last transform edit (Ctrl+Shift+Z)");

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

    if (m_Project)
    {
        ImGui::SameLine(0.0f, 10.0f);
        EditorUI::Separator();
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::TextDisabled("Project: %s", m_Project->Name.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nAssets: %s", m_Project->RootPath.string().c_str(),
                              m_Project->AssetPath.string().c_str());
    }

    ImGui::SameLine(0.0f, 10.0f);
    EditorUI::Separator();
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::TextDisabled("Entities: %zu", m_ActiveScene ? m_ActiveScene->GetRegistry().view<entt::entity>().size() : 0);

    ImGui::SameLine(0.0f, 12.0f);
    if (m_SceneHireacyPanel.GetSelectedEntity() &&
        m_SceneHireacyPanel.GetSelectedEntity().HasComponent<TagComponent>())
        ImGui::TextDisabled("Selected: %s",
                            m_SceneHireacyPanel.GetSelectedEntity().GetComponent<TagComponent>().Tag.c_str());
    else if (m_HoverdEntity && m_HoverdEntity.HasComponent<TagComponent>())
        ImGui::TextDisabled("Hover: %s", m_HoverdEntity.GetComponent<TagComponent>().Tag.c_str());
    else
        ImGui::TextDisabled("Hover: -");

    char performance[128];
    const float fps = ImGui::GetIO().Framerate;
    std::snprintf(performance, sizeof(performance), "%.0f FPS  |  %.2f ms  |  %s", fps,
                  1000.0f / (fps > 0.0f ? fps : 1.0f), EditorTheme::GetName(EditorTheme::GetSelected()));
    const float performanceWidth = ImGui::CalcTextSize(performance).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - performanceWidth - 12.0f);
    ImGui::TextDisabled("%s", performance);

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

    EditorUI::SectionHeader("Viewport");
    float cameraSpeed = m_EditorCamera.GetSpeedMultiplier();
    if (ImGui::SliderFloat("Camera Speed", &cameraSpeed, 0.1f, 5.0f, "%.2fx"))
        m_EditorCamera.SetSpeedMultiplier(cameraSpeed);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pan, orbit and zoom sensitivity (also in the viewport HUD).");

    static const char* kSnapLabels[] = {"0.1", "0.25", "0.5", "1", "2", "5", "10"};
    static const float kSnapValues[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f};
    int snapIndex = 2;
    for (int i = 0; i < 7; ++i)
    {
        if (std::abs(kSnapValues[i] - m_SnapValue) < 0.001f)
        {
            snapIndex = i;
            break;
        }
    }
    if (ImGui::Combo("Snap Increment", &snapIndex, kSnapLabels, 7))
        m_SnapValue = kSnapValues[snapIndex];

    ImGui::Checkbox("Viewport Overlay", &m_ShowViewportOverlay);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Tool badges, orientation gizmo and camera controls drawn over the viewport.");
    ImGui::SameLine();
    if (ImGui::Button("Focus Selected (F)"))
        FocusSelectedEntity();

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

    if (m_Project)
    {
        EditorUI::SectionHeader("Project");
        ImGui::TextDisabled("%s", m_Project->Name.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nAssets: %s", m_Project->RootPath.string().c_str(),
                              m_Project->AssetPath.string().c_str());

        // Start scene, relative to the project root (like a default map).
        char startScene[256];
        std::snprintf(startScene, sizeof(startScene), "%s", m_Project->StartScene.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##StartScene", "Assets/Scenes/Main.unhuman", startScene, sizeof(startScene),
                                     ImGuiInputTextFlags_EnterReturnsTrue))
        {
            m_Project->StartScene = startScene;
            std::string error;
            if (!Project::Save(*m_Project, &error))
                UHE_CORE_WARN("Could not save the project manifest: {0}", error);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Opened automatically the next time this project is opened");

        const bool canUseCurrentScene = !m_CurrentScenePath.empty();
        ImGui::BeginDisabled(!canUseCurrentScene);
        if (ImGui::Button("Use Current Scene") && canUseCurrentScene)
        {
            std::error_code ec;
            std::filesystem::path relative = std::filesystem::relative(m_CurrentScenePath, m_Project->RootPath, ec);
            m_Project->StartScene = ec ? m_CurrentScenePath.string() : relative.generic_string();
            std::string error;
            if (!Project::Save(*m_Project, &error))
                UHE_CORE_WARN("Could not save the project manifest: {0}", error);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save the current scene inside the project first");

        ImGui::SameLine();
        if (ImGui::Button("Close Project"))
            CloseProject();
    }

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
