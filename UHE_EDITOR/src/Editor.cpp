#include "Editor.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <string>
#include <utility>
#include <vector>
#include "EditorTheme.h"
#include "EditorUI.h"
#include "ImGuizmo.h"
#include "UHE/AssetsManager/AssetImport.h"
#include "UHE/AssetsManager/GameExporter.h"
#include "UHE/AssetsManager/PrimitiveGltf.h"
#include "UHE/AssetsManager/Project.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Audio/AudioEngine.h"
#include "UHE/Core/CommandLine.h"
#include "UHE/Core/Console.h"
#include "UHE/Debug/RenderDoc.h"
#include "UHE/Math/Math.h"
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHITexture.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer/UIRenderer.h"
#include "UHE/Renderer3D/Renderer3D.h"
#include "UHE/Renderer3D/SceneRenderer.h"
#include "UHE/Scripting/ScriptEngine.h"

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
    // Window mode, size and VSync before the first frame, so it opens as it was left.
    m_DisplaySettings.Load();
    m_DisplaySettings.ApplyAll(Application::Get().GetWindow());

    // Shader and script edits show up without a restart while the editor is open;
    // a game build leaves this off, since it ships no sources to watch.
    ShaderCache::Get().SetHotReloadEnabled(true);
    ScriptEngine::Get().SetHotReloadEnabled(true);

    // A shader edit that does not compile keeps the last good version drawing;
    // say so on screen, not only in the log.
    ShaderCache::Get().AddFailureListener(
        [this](ShaderCache::Handle handle, const std::string& error)
        {
            const ShaderCache::Shader* shader = ShaderCache::Get().Get(handle);
            const std::string name =
                shader ? std::filesystem::path(shader->SourcePath).filename().string() : std::string("shader");
            const std::string firstLine = error.substr(0, error.find('\n'));
            Notify(NotificationCenter::Kind::Error, name + " failed to compile", firstLine);
        });

    m_ActiveScene = CreateRef<Scene>();
    m_EditorCamera = EditorCamera(45.0f, 1.766f, 0.1f, 1000.0f);
    // Start above the ground and at an angle, as Blender and Godot do: level
    // with the grid, the plane is edge-on and the view shows nothing.
    m_EditorCamera.SetDistance(15.0f);
    m_EditorCamera.SetPitch(0.45f);
    m_EditorCamera.SetYaw(-0.6f);
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
            // Compare path elements, not native() text: that is wchar_t on Windows.
            if (!ec && !relative.empty() && *relative.begin() != "..")
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

    // An explicit -new-project or -project wins. Otherwise the most recent project that is still
    // on disk is reopened, so relaunching the editor lands back in the project
    // that was open rather than making the user find it again. `-launcher` asks
    // for the manager instead, and File > Project Manager... returns to it from
    // inside a project.
    const std::string requestedProject = CommandLine::GetValue("-project");
    // -new-project <folder>: what the manager's Create does, with the starter scene.
    const std::string newProject = CommandLine::GetValue("-new-project");
    if (!newProject.empty())
    {
        const std::filesystem::path root = Project::NormalizeRoot(newProject);
        const std::string problem = Project::CheckNewProject(root.parent_path(), root.filename().string());
        ProjectInfo created;
        std::string error;
        if (!problem.empty())
            m_ProjectLauncher.SetMessage("Could not create '" + root.string() + "': " + problem);
        else if (!Project::Create(root, root.filename().string(), created, &error))
            m_ProjectLauncher.SetMessage("Could not create '" + root.string() + "': " + error);
        else
            SetProject(std::move(created), true);
    }
    else if (!requestedProject.empty())
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

    RegisterConsoleEntries();

    // -exec "<console line>[; <console line>...]": run console commands at start-up,
    // for scripted runs (a capture of one buffer view, a fixed tick rate).
    const std::string startup = CommandLine::GetValue("-exec");
    size_t begin = 0;
    while (begin < startup.size())
    {
        const size_t end = std::min(startup.find(';', begin), startup.size());
        const std::string line = startup.substr(begin, end - begin);
        if (line.find_first_not_of(' ') != std::string::npos)
            UHE_CORE_INFO("-exec {0}: {1}", line, Console::Get().Execute(line));
        begin = end + 1;
    }
}

void Editor::OnDetach()
{
    m_DisplaySettings.CaptureWindowSize(Application::Get().GetWindow());
    m_DisplaySettings.Save();
    UnregisterConsoleEntries();
}

namespace
{
/// @brief Every console entry the editor owns, so OnDetach removes exactly these.
constexpr const char* kEditorConsoleEntries[] = {"script.budget", "sim.tickrate", "gpu.capture", "view.shading",
                                                 "view.buffer",   "scene.stats",  "clear"};
} // namespace

void Editor::RegisterConsoleEntries()
{
    Console& console = Console::Get();

    console.RegisterVar("script.budget", static_cast<i32>(ScriptEngine::kDefaultInstructionBudget / 1'000'000),
                        "million Lua instructions one script call may run; 0 = unlimited",
                        [](const Console::Value& value)
                        {
                            const i32 millions = std::max(0, std::get<i32>(value));
                            ScriptEngine::Get().SetInstructionBudget(static_cast<u64>(millions) * 1'000'000);
                        });

    console.RegisterCommand("sim.tickrate", "sim.tickrate [hz]: print or set the active scene's fixed tick rate",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                if (!m_ActiveScene)
                                    return "no active scene";
                                SimulationClock& clock = m_ActiveScene->GetSimulationClock();
                                if (!args.empty())
                                {
                                    int hz = 0;
                                    const std::string& text = args.front();
                                    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), hz);
                                    if (ec != std::errc() || end != text.data() + text.size() || hz <= 0 || hz > 10'000)
                                        return "tick rate must be 1..10000 Hz";
                                    clock.SetTickRate(static_cast<u32>(hz));
                                }
                                char text[64];
                                std::snprintf(text, sizeof(text), "sim.tickrate = %.0f Hz", 1.0 / clock.GetFixedStep());
                                return text;
                            });

    console.RegisterCommand("gpu.capture",
                            "gpu.capture [note]: capture the next frame with RenderDoc (start with -renderdoc); the "
                            "note is saved in the capture's comments",
                            [](const std::vector<std::string>& args) -> std::string
                            {
                                if (!RenderDoc::Available())
                                    return "RenderDoc is not attached; start with -renderdoc";
                                std::string note;
                                for (const std::string& word : args)
                                    note += (note.empty() ? "" : " ") + word;
                                RenderDoc::TriggerCapture(note);
                                return "capturing the next frame";
                            });

    console.RegisterCommand(
        "view.shading", "view.shading [lit|unlit|solid|normals|wireframe|overdraw|lights]: the viewport's shading mode",
        [this](const std::vector<std::string>& args) -> std::string
        {
            static constexpr const char* kNames[] = {"lit",       "unlit",    "solid", "normals",
                                                     "wireframe", "overdraw", "lights"};
            if (!args.empty())
            {
                const auto* found = std::find_if(std::begin(kNames), std::end(kNames),
                                                 [&](const char* name) { return args.front() == name; });
                if (found == std::end(kNames))
                    return "expected lit, unlit, solid, normals, wireframe, overdraw or lights";
                SetViewMode(static_cast<Renderer3D::ViewMode>(found - std::begin(kNames)));
            }
            return std::string("view.shading = ") + kNames[std::clamp(static_cast<int>(m_ViewMode), 0, 6)];
        });

    console.RegisterCommand("view.buffer", "view.buffer [image|ao|depth|bloom]: what the lit viewport shows",
                            [this](const std::vector<std::string>& args) -> std::string
                            {
                                static constexpr const char* kNames[] = {"image", "ao", "depth", "bloom"};
                                if (!args.empty())
                                {
                                    const auto* found =
                                        std::find_if(std::begin(kNames), std::end(kNames),
                                                     [&](const char* name) { return args.front() == name; });
                                    if (found == std::end(kNames))
                                        return "expected image, ao, depth or bloom";
                                    m_DebugBuffer = static_cast<int>(found - std::begin(kNames));
                                }
                                return std::string("view.buffer = ") + kNames[std::clamp(m_DebugBuffer, 0, 3)];
                            });

    console.RegisterCommand("scene.stats", "entity count and simulation tick of the active scene",
                            [this](const std::vector<std::string>&) -> std::string
                            {
                                if (!m_ActiveScene)
                                    return "no active scene";
                                const auto entities = m_ActiveScene->GetRegistry().view<TagComponent>().size();
                                return std::to_string(entities) + " entities, tick " +
                                       std::to_string(m_ActiveScene->GetSimulationClock().GetTickIndex());
                            });

    console.RegisterCommand("clear", "clear the console log",
                            [](const std::vector<std::string>&)
                            {
                                UHE::Log::GetImGuiSink()->Clear();
                                return std::string();
                            });
}

void Editor::UnregisterConsoleEntries()
{
    for (const char* name : kEditorConsoleEntries)
        Console::Get().Unregister(name);
}

void Editor::OnUpdate(UHE::Timestep ts)
{
    UHE_PROFILE_FUNCTION();
    // Before this frame's UI: a window mode switch resizes the framebuffer.
    m_DisplaySettings.ApplyPending(Application::Get().GetWindow());
    m_Notifications.Update(ts.GetSeconds());
    m_SecondsSinceTransformPush += ts.GetSeconds();
    m_CameraController.OnUpdate(ts);
    // The scene targets follow the viewport panel times the render scale: the
    // frame graph creates them at this size, so a resize or a new scale is a
    // new size here and nothing else. The panel still shows them at its size.
    const u32 sceneWidth = m_DisplaySettings.ScaledSize(m_ViewPortSize.x, kMaxSceneSize);
    const u32 sceneHeight = m_DisplaySettings.ScaledSize(m_ViewPortSize.y, kMaxSceneSize);
    if (m_ViewPortSize.x > 0.0f && m_ViewPortSize.y > 0.0f &&
        (sceneWidth != m_SceneWidth || sceneHeight != m_SceneHeight))
    {
        m_SceneWidth = sceneWidth;
        m_SceneHeight = sceneHeight;
        m_CameraController.OnResize(m_ViewPortSize.x, m_ViewPortSize.y);
        m_EditorCamera.SetViewportSize(m_ViewPortSize.x, m_ViewPortSize.y);
        m_ActiveScene->OnViewportResize(m_SceneWidth, m_SceneHeight);
        m_FramesSinceResize = 0;
    }
    else
    {
        m_FramesSinceResize++;
    }

    const bool runtimeScene = m_SceneState != SceneState::Edit;

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
            m_ActiveScene->OnUpdateEditorSimulation(ts);
            break;
        }
        case SceneState::Play:
        {
            m_ActiveScene->OnUpdateRuntimeSimulation(ts);
            break;
        }
        case SceneState::Paused:
        {
            // Let the editor camera fly around while the simulation is frozen.
            m_EditorCamera.OnUpdate(ts);
            if (m_StepRequested)
            {
                // Issue #49: a step is exactly one fixed simulation tick, not a
                // frame of wall time, so stepping is reproducible.
                m_ActiveScene->StepSimulation();
                m_StepRequested = false;
            }
            else
            {
                renderTs = 0.0f; // the scene pass redraws the frozen state
            }
            break;
        }
    }

    // Recorded when the device realizes the frame graph, before the UI pass,
    // which samples the scene colour this pass renders.
    DeclareSceneGraphPass(renderTs, runtimeScene);

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
    using namespace RHI;
    FrameGraph* graph = Renderer::GetDevice().GetFrameGraph();
    if (graph == nullptr || !m_ActiveScene)
        return;

    const u32 width = m_SceneWidth;
    const u32 height = m_SceneHeight;
    if (width == 0 || height == 0)
        return; // the viewport has not been laid out yet
    m_SceneTime += ts.GetSeconds();

    // Every target is the graph's: it allocates them at this size and retires
    // the old size after a resize.
    SceneRenderer::Desc desc;
    desc.Width = width;
    desc.Height = height;
    desc.SceneData = m_ActiveScene;
    desc.Runtime = runtime;
    desc.Lit = m_ViewMode == Renderer3D::ViewMode::Lit;
    desc.DebugBuffer = m_DebugBuffer;
    desc.Time = m_SceneTime;
    // Editing clears to a neutral grey, like Blender's and Godot's viewports,
    // so the grid and dark objects read; the running game keeps its own black.
    desc.ClearColor =
        runtime ? std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f} : std::array<float, 4>{0.038f, 0.040f, 0.046f, 1.0f};
    desc.ColorName = kSceneColor;
    desc.EntityName = kSceneEntity;
    // The scene this frame was declared with, not whatever m_ActiveScene is when
    // the passes run: Play and Stop in the UI swap it in between.
    desc.ResolveView = [this, runtime, scene = m_ActiveScene]()
    {
        if (runtime)
        {
            if (std::optional<SceneRenderView> view = scene->GetPrimaryCameraView())
                return *view;
        }
        SceneRenderView view;
        view.View = m_EditorCamera.GetViewMatrix();
        view.Projection = m_EditorCamera.GetProjection();
        view.Position = m_EditorCamera.GetPosition();
        return view;
    };
    desc.DrawScene = [this, ts, runtime, scene = m_ActiveScene](FGContext& context)
    { RecordSceneGraphPass(context, *scene, ts, runtime); };
    // Lit and editing: the grid goes over the water, as an overlay.
    if (desc.Lit && !runtime)
        desc.DrawOverlays = [scene = m_ActiveScene](FGContext&) { scene->RenderEditorGrid(); };
    const SceneRenderer::Output scene = SceneRenderer::AddPasses(*graph, desc);
    if (!scene.Color.IsValid())
        return;

    // The game UI over the viewport, in edit mode too (a preview of the
    // layout); it takes the mouse only in Play. The viewport image is shown
    // flipped (ImGui::Image with swapped v), so the UI is drawn flipped.
    const bool mouseDown = Input::IsMouseButtonPressed(UHE_MOUSE_BUTTON_LEFT);
    UI::Pointer pointer;
    pointer.Active = runtime && m_ViewportPointerInside;
    pointer.Position = m_ViewportPointer;
    pointer.Down = mouseDown;
    pointer.Pressed = mouseDown && !m_UIMouseWasDown;
    pointer.Released = !mouseDown && m_UIMouseWasDown;
    m_UIMouseWasDown = mouseDown;
    m_ActiveScene->UpdateUI(glm::vec2(static_cast<f32>(width), static_cast<f32>(height)), pointer);
    const FGHandle<FGTexture> color = m_ShowGameUI
                                          ? UIRenderer::AddPass(*graph, scene.Color, TextureFormat::RGBA8_SRGB, width,
                                                                height, true, m_ActiveScene->GetUIDrawList())
                                          : scene.Color;

    // Read after the frame: the viewport panel samples the colour, and
    // picking copies one pixel out of the entity IDs.
    graph->Export(color);
    graph->Export(scene.Entity, {.FinalState = FGState::ShaderRead, .CopySource = true});
}

void Editor::RecordSceneGraphPass(RHI::FGContext& context, Scene& scene, Timestep ts, bool runtime)
{
    if (context.Width() == 0 || context.Height() == 0)
        return;

    // The shading mode is the viewport's, not the scene's: set for this pass
    // only, so nothing else that renders models inherits it.
    Renderer3D::SetViewMode(m_ViewMode);
    if (runtime)
        scene.RenderRuntimeScene(ts);
    else
        scene.RenderEditorScene(ts, m_EditorCamera, m_ViewMode != Renderer3D::ViewMode::Lit);
    Renderer3D::SetViewMode(Renderer3D::ViewMode::Lit);
}

void Editor::SetViewMode(Renderer3D::ViewMode mode)
{
    if (mode == m_ViewMode)
        return;
    if (mode == Renderer3D::ViewMode::Wireframe)
        m_ViewModeBeforeWireframe = m_ViewMode;
    m_ViewMode = mode;
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
    const bool overViewport = e.GetX() >= viewportMin.x && e.GetX() <= viewportMax.x && e.GetY() >= viewportMin.y &&
                              e.GetY() <= viewportMax.y;

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
    const std::string storedPath =
        m_Project ? MakeAssetRelativePath(assetPath, m_Project->AssetPath) : assetPath.generic_string();
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
    const float extent =
        std::max({std::abs(transform.Scale.x), std::abs(transform.Scale.y), std::abs(transform.Scale.z)});

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
    const u32 failed = ShaderCache::Get().ReloadAll();
    if (failed == 0)
    {
        Notify(NotificationCenter::Kind::Success, "Shaders reloaded",
               "Every loaded shader was recompiled and its pipelines rebuilt.");
        return;
    }
    // Each failure already raised its own toast through the failure listener.
    Notify(NotificationCenter::Kind::Warning, "Shaders reloaded with errors",
           std::to_string(failed) + " shader(s) kept their last good version; see the console.");
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
                         m_TransformBaseline.Rotation != current.Rotation || m_TransformBaseline.Scale != current.Scale;
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
            else if (shift)
            {
                // Blender's Shift+Z: wireframe on, and back to the previous mode.
                SetViewMode(m_ViewMode == Renderer3D::ViewMode::Wireframe ? m_ViewModeBeforeWireframe
                                                                          : Renderer3D::ViewMode::Wireframe);
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

void Editor::SetProject(ProjectInfo info, bool starterScene)
{
    std::string error;
    if (!Project::EnsureAssetFolder(info, &error))
        UHE_CORE_WARN("Project '{0}': {1}", info.Name, error);
    if (starterScene)
        WriteStarterScene(info);

    // "game:" -> the project's Assets folder, "engine:" -> the folder shipped
    // next to the binary. Mounting the project last keeps it ahead of the engine
    // assets, so a project can shadow an engine file by name.
    Project::Mount(info, FileSystem::Get().GetRootPath() / "assets");

    m_Project = std::move(info);
    m_ShowLauncher = false;
    m_ProjectLauncher.AddRecent(*m_Project);
    m_ContentBrowserPanel.SetBaseDirectory(m_Project->AssetPath);

    const std::filesystem::path startScene = Project::StartScenePath(*m_Project);
    // Read before opening the scene: the messages below must not depend on
    // the scene calls leaving m_Project alone.
    const std::string projectName = m_Project->Name;
    const std::string projectRoot = m_Project->RootPath.string();
    std::error_code ec;
    if (!startScene.empty() && std::filesystem::is_regular_file(startScene, ec))
        OpenScene(startScene, false);
    else
        NewScene(false);

    UHE_CORE_INFO("Opened project '{0}' ({1})", projectName, projectRoot);
    Notify(NotificationCenter::Kind::Success, "Project opened", projectName);
}

bool Editor::WriteStarterScene(ProjectInfo& project)
{
    // Its own shapes rather than the engine's sample models: the project
    // keeps working, and exports, without anything outside its folder.
    const std::filesystem::path models = project.AssetPath / "Models";
    std::string error;
    if (!WritePrimitiveGltf(models / "Ground.gltf", PrimitiveShape::Plane, {0.20f, 0.30f, 0.15f, 1.0f}, 0.9f, &error) ||
        !WritePrimitiveGltf(models / "Cube.gltf", PrimitiveShape::Cube, {0.75f, 0.26f, 0.10f, 1.0f}, 0.5f, &error))
    {
        UHE_CORE_ERROR("Starter scene for '{0}': {1}", project.Name, error);
        return false;
    }

    Ref<Scene> scene = CreateRef<Scene>();
    const auto place = [&](const char* name, glm::vec3 position, glm::vec3 rotation, glm::vec3 scale)
    {
        Entity entity = scene->CreateEntity(name);
        auto& transform = entity.GetComponent<TransformComponent>();
        transform.Translation = position;
        transform.Rotation = rotation;
        transform.Scale = scale;
        return entity;
    };
    place("Ground", glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(30.0f, 1.0f, 30.0f))
        .AddComponent<Model3DComponent>()
        .ModelPath = "Models/Ground.gltf";
    place("Cube", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 0.2f, 0.0f), glm::vec3(1.0f))
        .AddComponent<Model3DComponent>()
        .ModelPath = "Models/Cube.gltf";
    // From the side of the view, not from behind the camera: light from
    // behind lights every face alike and hides the cube's shadow behind it.
    place("Sun", glm::vec3(0.0f, 6.0f, 0.0f), glm::vec3(-0.8f, 2.4f, 0.0f), glm::vec3(1.0f))
        .AddComponent<DirectionalLightComponent>()
        .Intensity = 1.6f;
    // Up and to the side, looking at the cube.
    const glm::vec3 eye(4.0f, 2.6f, 6.0f);
    const float yaw = std::atan2(eye.x, eye.z);
    const float pitch = -std::atan2(eye.y - 0.5f, glm::length(glm::vec2(eye.x, eye.z)));
    auto& camera = place("Camera", eye, glm::vec3(pitch, yaw, 0.0f), glm::vec3(1.0f)).AddComponent<CameraComponent>();
    camera.Camera.SetProjectionType(SceneCamera::ProjectionType::Perpective);
    camera.Primary = true;

    const std::filesystem::path scenePath = project.AssetPath / "Scenes" / "Main.unhuman";
    std::error_code ec;
    std::filesystem::create_directories(scenePath.parent_path(), ec);
    SceneSerializer(scene).Serialize(scenePath.string());
    if (!std::filesystem::is_regular_file(scenePath, ec))
    {
        UHE_CORE_ERROR("Starter scene for '{0}': could not write '{1}'", project.Name, scenePath.string());
        return false;
    }

    project.StartScene = std::filesystem::relative(scenePath, project.RootPath, ec).generic_string();
    if (!Project::Save(project, &error))
        UHE_CORE_WARN("Starter scene for '{0}': the manifest was not saved: {1}", project.Name, error);
    return true;
}

void Editor::CloseProject()
{
    // Nothing of the project keeps running behind the manager: Play stops
    // (scripts, physics, audio) and its scene is let go.
    OnSceneStop();
    m_ActiveScene = CreateRef<Scene>();
    m_EditorScene = m_ActiveScene;
    m_SceneHireacyPanel.SetContext(m_ActiveScene);
    m_CurrentScenePath.clear();
    m_Project.reset();
    m_ProjectLauncher.Refresh();
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

namespace
{
/// @brief Lines entered in the console, oldest first, for Up/Down recall.
std::vector<std::string> s_ConsoleHistory;
/// @brief Position while browsing the history; -1 when editing a new line.
int s_ConsoleHistoryPos = -1;

/**
 * @brief Up/Down walk the history; Tab completes the name being typed.
 */
int ConsoleInputCallback(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && !s_ConsoleHistory.empty())
    {
        const int last = static_cast<int>(s_ConsoleHistory.size()) - 1;
        if (data->EventKey == ImGuiKey_UpArrow)
            s_ConsoleHistoryPos = s_ConsoleHistoryPos < 0 ? last : std::max(0, s_ConsoleHistoryPos - 1);
        else if (data->EventKey == ImGuiKey_DownArrow && s_ConsoleHistoryPos >= 0)
            s_ConsoleHistoryPos = s_ConsoleHistoryPos >= last ? -1 : s_ConsoleHistoryPos + 1;

        data->DeleteChars(0, data->BufTextLen);
        if (s_ConsoleHistoryPos >= 0)
            data->InsertChars(0, s_ConsoleHistory[s_ConsoleHistoryPos].c_str());
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
    {
        const std::string typed(data->Buf, data->BufTextLen);
        if (typed.find(' ') != std::string::npos)
            return 0; // only the name completes
        const std::vector<std::string> names = UHE::Console::Get().Complete(typed);
        if (names.size() == 1)
        {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, (names.front() + " ").c_str());
        }
        else if (!names.empty())
        {
            std::string list;
            for (const std::string& name : names)
                list += name + "  ";
            UHE_CORE_INFO("{}", list);
        }
    }
    return 0;
}

/**
 * @brief The command line under the log: runs a line through UHE::Console and
 * logs both the line and its result, so they land in the log like any output.
 */
void DrawConsoleInput()
{
    static char s_Line[512] = "";
    ImGui::SetNextItemWidth(-FLT_MIN);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
                                      ImGuiInputTextFlags_CallbackCompletion;
    if (ImGui::InputTextWithHint("##ConsoleInput", "> command or variable (help, Tab completes)", s_Line,
                                 sizeof(s_Line), flags, &ConsoleInputCallback))
    {
        const std::string line = s_Line;
        s_Line[0] = '\0';
        s_ConsoleHistoryPos = -1;
        if (!UHE::Console::Tokenize(line).empty())
        {
            if (s_ConsoleHistory.empty() || s_ConsoleHistory.back() != line)
                s_ConsoleHistory.push_back(line);
            UHE_CORE_INFO("> {}", line);
            const std::string result = UHE::Console::Get().Execute(line);
            if (!result.empty())
                UHE_CORE_INFO("{}", result);
        }
        ImGui::SetKeyboardFocusHere(-1); // keep typing after Enter
    }
}
} // namespace

namespace
{
/**
 * @brief A level filter as a compact chip: coloured dot, count, accent outline while on.
 * @return true when clicked (the caller toggles the filter).
 */
bool LevelChip(const char* id, const char* tooltip, size_t count, const ImVec4& color, bool enabled)
{
    char label[32];
    std::snprintf(label, sizeof(label), "%zu", count);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float dot = 4.0f;
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 size(style.FramePadding.x * 2.0f + dot * 2.0f + 6.0f + textSize.x, ImGui::GetFrameHeight());
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const float rounding = style.FrameRounding;
    if (enabled)
        draw->AddRectFilled(pos, max, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    if (hovered)
        draw->AddRectFilled(pos, max, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), rounding);
    const ImVec4 tint = enabled ? color : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const float midY = pos.y + size.y * 0.5f;
    draw->AddCircleFilled(ImVec2(pos.x + style.FramePadding.x + dot, midY), dot, ImGui::GetColorU32(tint), 12);
    draw->AddText(ImVec2(pos.x + style.FramePadding.x + dot * 2.0f + 6.0f, midY - textSize.y * 0.5f),
                  ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
    if (hovered)
        ImGui::SetTooltip("%s (%s)", tooltip, enabled ? "shown, click to hide" : "hidden, click to show");
    return clicked;
}

/// @brief The first line of \p text; \p extra gets how many more lines follow.
std::string_view FirstLine(const std::string& text, int& extra)
{
    const size_t end = text.find('\n');
    extra = 0;
    if (end == std::string::npos)
        return text;
    for (size_t i = end; i < text.size(); ++i)
        extra += (text[i] == '\n' && i + 1 < text.size()) ? 1 : 0;
    return std::string_view(text).substr(0, end);
}
} // namespace

void DrawConsolePanel()
{
    UHE_PROFILE_FUNCTION();
    ImGui::Begin("Console");

    auto sink = UHE::Log::GetImGuiSink();
    std::vector<UHE::ImGuiLogEntry> logs = sink->GetSnapshot();

    static constexpr int kLevelCount = 6;
    static const char* kLevelNames[kLevelCount] = {"Trace", "Debug", "Info", "Warnings", "Errors", "Critical"};
    // Trace and debug are noise most of the time: off by default, like Godot's output.
    static bool s_LevelEnabled[kLevelCount] = {false, true, true, true, true, true};
    static char s_Search[256] = "";
    static bool s_AutoScroll = true;
    static bool s_ShowTime = true;
    static bool s_ShowSource = true;

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

    // Theme-aware palette: info keeps the normal text colour, so only what
    // needs attention is coloured.
    ImVec4 debugColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    debugColor.w *= 0.65f;
    const ImVec4 colors[kLevelCount] = {
        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
        debugColor,
        ImGui::GetStyleColorVec4(ImGuiCol_Text),
        EditorTheme::ConsoleWarn(),
        EditorTheme::ConsoleError(),
        EditorTheme::ConsoleCritical(),
    };

    auto toLower = [](std::string value)
    {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    const std::string needle = toLower(s_Search);

    std::vector<const UHE::ImGuiLogEntry*> visible;
    visible.reserve(logs.size());
    for (const auto& log : logs)
    {
        if (!s_LevelEnabled[levelIndex(log.Level)])
            continue;
        if (!needle.empty() && toLower(log.Message).find(needle) == std::string::npos)
            continue;
        visible.push_back(&log);
    }

    // ── Toolbar: Clear | filter | level chips | options ─────────────────────
    if (ImGui::Button("Clear"))
        sink->Clear();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Remove every message from the console");
    ImGui::SameLine();

    // Chips for info, warnings and errors (errors include critical); trace
    // and debug live in the options menu.
    const float chipsWidth = 3.0f * 64.0f;
    const float optionsWidth = ImGui::CalcTextSize("...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - chipsWidth - optionsWidth - 24.0f));
    ImGui::InputTextWithHint("##ConsoleFilter", "Filter messages", s_Search, sizeof(s_Search));

    ImGui::SameLine();
    if (LevelChip("##ChipInfo", "Info", counts[2], colors[2], s_LevelEnabled[2]))
        s_LevelEnabled[2] = !s_LevelEnabled[2];
    ImGui::SameLine(0.0f, 2.0f);
    if (LevelChip("##ChipWarn", "Warnings", counts[3], colors[3], s_LevelEnabled[3]))
        s_LevelEnabled[3] = !s_LevelEnabled[3];
    ImGui::SameLine(0.0f, 2.0f);
    if (LevelChip("##ChipError", "Errors", counts[4] + counts[5], colors[4], s_LevelEnabled[4]))
        s_LevelEnabled[4] = s_LevelEnabled[5] = !s_LevelEnabled[4];

    ImGui::SameLine();
    if (ImGui::Button("..."))
        ImGui::OpenPopup("##ConsoleOptions");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Console options");
    if (ImGui::BeginPopup("##ConsoleOptions"))
    {
        ImGui::SeparatorText("Show");
        for (int i = 0; i < kLevelCount; ++i)
        {
            char label[64];
            std::snprintf(label, sizeof(label), "%s (%zu)", kLevelNames[i], counts[i]);
            ImGui::Checkbox(label, &s_LevelEnabled[i]);
        }
        ImGui::SeparatorText("Columns");
        ImGui::Checkbox("Time", &s_ShowTime);
        ImGui::Checkbox("Source", &s_ShowSource);
        ImGui::SeparatorText("Behaviour");
        ImGui::Checkbox("Auto-scroll", &s_AutoScroll);
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Visible Messages", nullptr, false, !visible.empty()))
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
        ImGui::TextDisabled("%zu of %zu shown", visible.size(), logs.size());
        ImGui::TextDisabled("Log file: %s", UHE::Log::GetLogFilePath().c_str());
        ImGui::EndPopup();
    }

    // ── Log: one row per message, clipped to what is on screen ─────────────
    // Leave one row under the log for the command line.
    const float inputHeight = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::BeginChild("##ConsoleLogRegion", ImVec2(0, -inputHeight), ImGuiChildFlags_None);
    ImGui::PopStyleColor();

    if (visible.empty())
    {
        const char* hint = logs.empty() ? "No messages" : "No message matches the filters";
        const ImVec2 size = ImGui::CalcTextSize(hint);
        const ImVec2 region = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPos(ImVec2((region.x - size.x) * 0.5f, (region.y - size.y) * 0.5f));
        ImGui::TextDisabled("%s", hint);
    }
    else
    {
        const int columns = 1 + (s_ShowTime ? 1 : 0) + (s_ShowSource ? 1 : 0);
        const ImGuiTableFlags flags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadOuterX;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 2.0f));
        if (ImGui::BeginTable("##ConsoleLog", columns, flags))
        {
            if (s_ShowTime)
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00:00:00").x);
            if (s_ShowSource)
                ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("APP").x);
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const UHE::ImGuiLogEntry& entry = *visible[static_cast<size_t>(row)];
                    const int level = levelIndex(entry.Level);
                    ImGui::TableNextRow();
                    ImGui::PushID(row);

                    // Warnings and errors get a tinted row so they stand out in a long log.
                    if (level >= 3)
                    {
                        ImVec4 tint = colors[level];
                        tint.w = 0.10f;
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(tint));
                    }
                    if (s_ShowTime)
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", entry.Time.c_str());
                    }
                    if (s_ShowSource)
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", entry.Logger.c_str());
                    }
                    ImGui::TableNextColumn();
                    // One line per row keeps the clipper exact; a multi-line
                    // message (a Lua traceback) says how much more it has and
                    // shows all of it on hover.
                    const std::string& text = entry.Text.empty() ? entry.Message : entry.Text;
                    int extra = 0;
                    const std::string_view line = FirstLine(text, extra);
                    ImGui::PushStyleColor(ImGuiCol_Text, colors[level]);
                    ImGui::TextUnformatted(line.data(), line.data() + line.size());
                    ImGui::PopStyleColor();
                    const bool hovered = ImGui::IsItemHovered();
                    if (extra > 0)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(+%d lines)", extra);
                    }
                    if ((hovered || ImGui::IsItemHovered()) &&
                        (extra > 0 || ImGui::GetItemRectMax().x > ImGui::GetWindowPos().x + ImGui::GetWindowWidth()))
                    {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 50.0f);
                        ImGui::TextUnformatted(text.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    if (ImGui::BeginPopupContextItem("##LogLine"))
                    {
                        if (ImGui::MenuItem("Copy Message"))
                            ImGui::SetClipboardText(text.c_str());
                        if (ImGui::MenuItem("Copy Full Line"))
                            ImGui::SetClipboardText(entry.Message.c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        // Auto-scroll only when already pinned to the bottom, so inspecting
        // older lines is not interrupted by incoming logs.
        if (s_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    DrawConsoleInput();

    ImGui::End();
}

void Editor::ExportGame()
{
    if (!m_Project)
        return;
    const std::string folder = FileDialogs::SelectFolder("Export Game To", m_Project->RootPath.parent_path().string());
    if (folder.empty())
        return;

    ExportOptions options;
    options.OutputDir = folder;
    // The runtime is built next to the editor, into the same folder.
    options.RuntimeDir = CommandLine::GetExecutablePath().parent_path();
    options.EngineAssets = FileSystem::Get().GetRootPath() / "assets";
    const ExportReport report = GameExporter::Export(*m_Project, options);
    for (const std::string& warning : report.Warnings)
        UHE_CORE_WARN("Export: {0}", warning);
    if (report.Ok)
    {
        UHE_CORE_INFO("Exported '{0}': {1} files, {2} MB, run {3}", m_Project->Name, report.FilesCopied,
                      report.BytesCopied / (std::uintmax_t{1024} * 1024), report.Executable.string());
        if (report.Warnings.empty())
            Notify(NotificationCenter::Kind::Success, "Game exported", report.Executable.string());
        else
            Notify(NotificationCenter::Kind::Warning, "Game exported with warnings",
                   std::to_string(report.Warnings.size()) + " references will not load; see the console");
    }
    else
    {
        for (const std::string& error : report.Errors)
            UHE_CORE_ERROR("Export: {0}", error);
        Notify(NotificationCenter::Kind::Error, "Export failed", report.Errors.empty() ? "" : report.Errors.front());
    }
}

void Editor::SaveSceneAs()
{
    std::string filepath = FileDialogs::SaveFile("UHE Scene (*.unhuman)\0.unhuman\0", ProjectAssetPath().string());

    if (!filepath.empty())
    {
        SceneSerializer serializer(m_ActiveScene);
        serializer.Serialize(filepath);
        m_CurrentScenePath = filepath;
        Notify(NotificationCenter::Kind::Success, "Scene saved", std::filesystem::path(filepath).filename().string());
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
        ImGui::DockBuilderDockWindow("Environment", inspector);
        // A new tab bar selects its last tab; Properties is the one to see first.
        // The ID is ImGuiWindow::TabId, GetID("#TAB") seeded with the window's ID.
        if (ImGuiDockNode* inspectorNode = ImGui::DockBuilderGetNode(inspector))
            inspectorNode->SelectedTabId = ImHashStr("#TAB", 0, ImHashStr("Properties"));
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
        if (std::optional<ProjectLauncherPanel::Result> picked = m_ProjectLauncher.OnImGuiRender())
            SetProject(std::move(picked->Project), picked->Created && picked->StarterScene);
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

    // Set every frame, like the Scripts panel: play, stop, new and open all replace the active scene.
    m_EnvironmentPanel.SetContext(m_ActiveScene);
    if (m_ShowEnvironment)
        m_EnvironmentPanel.OnImGuiRender(m_ShowEnvironment);

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
    // The texture the scene pass renders this frame; it changes after a resize.
    RHI::TextureHandle sceneColor = Renderer::GetDevice().GetExportedTexture(kSceneColor);
    void* textureId =
        sceneColor != nullptr ? reinterpret_cast<RHI::RHITexture*>(sceneColor)->GetImGuiTextureID() : nullptr;
    if (textureId != nullptr)
        ImGui::Image(textureId, viewPortSize, ImVec2{0, 1}, ImVec2{1, 0});
    else
        ImGui::Dummy(viewPortSize); // keeps the layout, drop target and gizmo rect
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

    // The game UI's mouse, in scene pixels (the scene is the panel times the render scale).
    m_ViewportPointerInside = mouseX >= 0 && mouseY >= 0 && mouseX < (int)viewportSize.x &&
                              mouseY < (int)viewportSize.y && ImGui::IsWindowHovered();
    m_ViewportPointer = glm::vec2(mx * static_cast<f32>(m_SceneWidth) / std::max(viewportSize.x, 1.0f),
                                  my * static_cast<f32>(m_SceneHeight) / std::max(viewportSize.y, 1.0f));

    if (mouseX >= 0 && mouseY >= 0 && mouseX < (int)viewportSize.x && mouseY < (int)viewportSize.y)
    {
        // The entity IDs are the scene's size (panel times render scale) and
        // the image is stretched over the panel: scale the cursor the same way.
        const int pixelX = static_cast<int>(mx * static_cast<f32>(m_SceneWidth) / viewportSize.x);
        const int pixelY = static_cast<int>(my * static_cast<f32>(m_SceneHeight) / viewportSize.y);
        RHI::TextureHandle entityIds = Renderer::GetDevice().GetExportedTexture(kSceneEntity);
        if (m_FramesSinceResize > 1 && entityIds != nullptr && pixelX >= 0 && pixelY >= 0 &&
            std::cmp_less(pixelX, m_SceneWidth) && std::cmp_less(pixelY, m_SceneHeight))
        {
            // Async: the blocking read waited on the GPU every hovered frame.
            // -1 until the first read lands, and where no entity was drawn.
            int pixelData = -1;
            if (!Renderer::GetDevice().ReadPixelAsync(entityIds, pixelX, pixelY, &pixelData))
                pixelData = -1;
            if (pixelData < 0)
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
        draw->AddRect(badgeMin, badgeMax, ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.55f)), rounding);
        draw->AddCircleFilled(ImVec2(badgeMin.x + 9.0f, (badgeMin.y + badgeMax.y) * 0.5f), 3.0f,
                              ImGui::GetColorU32(accent));
        draw->AddText(ImVec2(badgeMin.x + 15.0f, badgeMin.y + 3.5f), IM_COL32(238, 238, 242, 255), text);

        badgeX = badgeMax.x + 6.0f;
    };

    if (m_ShowToolBadges)
    {
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
    }

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
        draw->AddText(ImVec2(warnMin.x + 12.0f, warnMin.y + 5.0f), ImGui::GetColorU32(EditorTheme::Warning()), warning);
    }

    // The orientation gizmo sits top-right, where Blender and Godot keep theirs.
    if (m_ShowNavigationGizmo)
        DrawAxisGizmo(draw, ImVec2(imageMax.x - 46.0f, imageMin.y + 46.0f), 28.0f);
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
        const float labelLength =
            std::max(std::sqrt(labelOffset.x * labelOffset.x + labelOffset.y * labelOffset.y), 0.001f);
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
    { commands.push_back({std::move(label), std::move(category), std::move(shortcut), enabled, std::move(action)}); };

    add("New Scene", "File", "Ctrl+N", true, [this] { NewScene(); });
    add("Open Scene...", "File", "Ctrl+O", true, [this] { OpenScene(); });
    add("Save Scene As...", "File", "Ctrl+S", true, [this] { SaveSceneAs(); });
    add("Export Game...", "File", "", m_Project.has_value(), [this] { ExportGame(); });
    add("Open Project...", "File", "", true,
        [this]
        {
            const std::string folder =
                FileDialogs::SelectFolder("Open Project", std::filesystem::current_path().string());
            if (!folder.empty() && !OpenProject(folder))
                m_ProjectLauncher.SetMessage("Could not open '" + folder + "'");
        });
    add("Project Manager...", "File", "", m_Project.has_value(), [this] { CloseProject(); });

    add("Undo Transform", "Edit", "Ctrl+Z", !m_UndoStack.empty(), [this] { UndoTransform(); });
    add("Redo Transform", "Edit", "Ctrl+Shift+Z", !m_RedoStack.empty(), [this] { RedoTransform(); });
    add("Delete Selected Entity", "Edit", "Del", m_SceneState == SceneState::Edit, [this] { DeleteSelectedEntity(); });
    add("Preferences...", "Edit", "Ctrl+,", true, [this] { m_ShowPreferences = true; });
    add("About UHE", "Help", "F1", true, [this] { m_ShowAbout = true; });

    add("Toggle Hierarchy", "View", "", true, [this] { m_ShowHierarchy = !m_ShowHierarchy; });
    add("Toggle Inspector", "View", "", true, [this] { m_ShowInspector = !m_ShowInspector; });
    add("Toggle Environment", "View", "", true, [this] { m_ShowEnvironment = !m_ShowEnvironment; });
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
    add("Step One Tick", "Play", "F7", m_SceneState == SceneState::Paused, [this] { OnScreenStep(); });
    add("Stop", "Play", "F5", m_SceneState != SceneState::Edit, [this] { OnSceneStop(); });

    add("Shading: Wireframe", "Viewport", "Shift+Z", m_ViewMode != Renderer3D::ViewMode::Wireframe,
        [this] { SetViewMode(Renderer3D::ViewMode::Wireframe); });
    add("Shading: Solid", "Viewport", "", m_ViewMode != Renderer3D::ViewMode::Solid,
        [this] { SetViewMode(Renderer3D::ViewMode::Solid); });
    add("Shading: Unlit", "Viewport", "", m_ViewMode != Renderer3D::ViewMode::Unlit,
        [this] { SetViewMode(Renderer3D::ViewMode::Unlit); });
    add("Shading: Lit", "Viewport", "", m_ViewMode != Renderer3D::ViewMode::Lit,
        [this] { SetViewMode(Renderer3D::ViewMode::Lit); });
    add("Shading: Normals", "Viewport", "", m_ViewMode != Renderer3D::ViewMode::Normals,
        [this] { SetViewMode(Renderer3D::ViewMode::Normals); });
    add("Shading: Overdraw", "Viewport", "", m_ViewMode != Renderer3D::ViewMode::Overdraw,
        [this] { SetViewMode(Renderer3D::ViewMode::Overdraw); });
    add("Shading: Light Complexity", "Viewport", "", m_ViewMode != Renderer3D::ViewMode::LightComplexity,
        [this] { SetViewMode(Renderer3D::ViewMode::LightComplexity); });
    add("Toggle Grid", "Viewport", "", true,
        [] { Scene::GetEditorOverlays().Grid = !Scene::GetEditorOverlays().Grid; });
    add("Toggle Light Icons", "Viewport", "", true,
        [] { Scene::GetEditorOverlays().LightIcons = !Scene::GetEditorOverlays().LightIcons; });
    add("Reload Shaders", "Render", "Ctrl+Shift+R", true, [this] { ReloadShaders(); });
    add("Reload Scripts", "Render", "Ctrl+Alt+R", true, [this] { ReloadScripts(); });

    for (int i = 0; i < EditorTheme::GetThemeCount(); ++i)
    {
        const auto theme = static_cast<EditorTheme::EditorThemeId>(i);
        const bool selected = EditorTheme::GetSelected() == theme;
        add(std::string("Theme: ") + EditorTheme::GetName(theme), "Appearance", selected ? "current" : "", !selected,
            [theme] { EditorTheme::SetSelected(theme); });
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
            const std::string folder =
                FileDialogs::SelectFolder("Open Project", std::filesystem::current_path().string());
            if (!folder.empty() && !OpenProject(folder))
                m_ProjectLauncher.SetMessage("Could not open '" + folder + "'");
        }
        if (ImGui::MenuItem("Project Manager..."))
            CloseProject();
        if (ImGui::MenuItem("Export Game...", nullptr, false, m_Project.has_value()))
            ExportGame();
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
        if (ImGui::MenuItem("Area Light"))
            CreateEntityFromMenu("Area Light", 7);
        ImGui::Separator();
        if (ImGui::MenuItem("Water"))
            CreateEntityFromMenu("Water", 8);
        if (ImGui::MenuItem("Wind"))
            CreateEntityFromMenu("Wind", 9);
        if (ImGui::MenuItem("Terrain"))
            CreateEntityFromMenu("Terrain", 10);
        ImGui::Separator();
        if (ImGui::BeginMenu("UI"))
        {
            // Inside the selected UI element, when one is selected.
            if (ImGui::MenuItem("Panel"))
                CreateUIElement(0);
            if (ImGui::MenuItem("Text"))
                CreateUIElement(1);
            if (ImGui::MenuItem("Button"))
                CreateUIElement(2);
            if (ImGui::MenuItem("Progress Bar"))
                CreateUIElement(3);
            if (ImGui::MenuItem("Slider"))
                CreateUIElement(4);
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        EditorUI::ToggleMenuItem("Hierarchy", &m_ShowHierarchy);
        EditorUI::ToggleMenuItem("Inspector", &m_ShowInspector);
        EditorUI::ToggleMenuItem("Environment", &m_ShowEnvironment);
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
        ImGui::SetTooltip("Advance one simulation tick while paused (F7)");

    if (playing || paused)
    {
        // Issue #49: simulated time is counted in ticks, independent of FPS.
        const SimulationClock& clock = m_ActiveScene->GetSimulationClock();
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f);
        ImGui::TextDisabled("tick %llu", static_cast<unsigned long long>(clock.GetTickIndex()));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%.3f s simulated at %.0f Hz", clock.GetSimulatedSeconds(), 1.0 / clock.GetFixedStep());
    }

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

    const f32 displayWidth = 88.0f + 12.0f + 1.0f + 12.0f + 62.0f;
    f32 clusterX = ImGui::GetWindowWidth() - 8.0f - displayWidth;
    const f32 minClusterX = ImGui::GetCursorPosX() + 16.0f;
    if (clusterX < minClusterX)
        clusterX = minClusterX;
    ImGui::SameLine(clusterX);
    ImGui::SetCursorPosY(5.0f);

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

    // Right side: View and Overlays menus, then the shading buttons. Laid out
    // from the right edge; a narrow viewport drops the menus before the buttons.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttonSize = ImGui::GetFrameHeight();
    const float shadingWidth = buttonSize * 5.0f + 2.0f * 4.0f;
    const float menusWidth = ImGui::CalcTextSize("View     ").x + ImGui::CalcTextSize("Overlays     ").x +
                             style.FramePadding.x * 4.0f + style.ItemSpacing.x + 14.0f;
    const float right = ImGui::GetWindowWidth() - 8.0f;
    const float available = right - ImGui::GetItemRectMax().x + ImGui::GetWindowPos().x - 16.0f;
    if (available > shadingWidth + menusWidth)
    {
        ImGui::SameLine(right - shadingWidth - menusWidth);
        ImGui::SetCursorPosY(3.0f);
        DrawViewportCameraMenu();
        ImGui::SameLine();
        DrawViewportOverlaysMenu();
        ImGui::SameLine(0.0f, 8.0f);
        EditorUI::Separator();
    }
    if (available > shadingWidth)
    {
        ImGui::SameLine(right - shadingWidth);
        ImGui::SetCursorPosY(3.0f);
        DrawViewportShading();
    }

    ImGui::EndChild();
}

void Editor::DrawViewportCameraMenu()
{
    if (!EditorUI::HeaderMenuButton("View", "##ViewportViewMenu", "Camera speed and framing"))
        return;
    if (!ImGui::BeginPopup("##ViewportViewMenu"))
        return;
    ImGui::SeparatorText("Camera");
    ImGui::SetNextItemWidth(160.0f);
    float speed = m_EditorCamera.GetSpeedMultiplier();
    if (ImGui::SliderFloat("Speed", &speed, 0.1f, 5.0f, "%.2fx"))
        m_EditorCamera.SetSpeedMultiplier(speed);
    if (ImGui::MenuItem("Frame Selected", "F"))
        FocusSelectedEntity();
    ImGui::SeparatorText("Buffer");
    // What the lit view shows: the image, or one of the buffers it is made from.
    static constexpr const char* kBuffers[] = {"Final Image", "Ambient Occlusion", "Depth", "Bloom"};
    for (int i = 0; i < 4; ++i)
    {
        if (ImGui::MenuItem(kBuffers[i], nullptr, m_DebugBuffer == i))
            m_DebugBuffer = i;
    }
    if (m_ViewMode != Renderer3D::ViewMode::Lit)
        ImGui::TextDisabled("Buffers show in the Lit shading mode.");
    ImGui::SeparatorText("GPU Capture");
    if (RenderDoc::Available())
    {
        if (ImGui::MenuItem("Capture Frame (RenderDoc)"))
            RenderDoc::TriggerCapture();
        ImGui::BeginDisabled(RenderDoc::CaptureCount() == 0);
        if (ImGui::MenuItem("Open Last Capture"))
            RenderDoc::LaunchReplayUI();
        ImGui::EndDisabled();
        ImGui::TextDisabled("%u captures", RenderDoc::CaptureCount());
    }
    else
    {
        ImGui::TextDisabled("Start the editor with -renderdoc to capture frames.");
    }
    ImGui::SeparatorText("Window");
    if (ImGui::MenuItem("Fullscreen Viewport", "Ctrl+Shift+F", m_IsViewportFullScreen))
        m_IsViewportFullScreen = !m_IsViewportFullScreen;
    ImGui::EndPopup();
}

void Editor::DrawViewportOverlaysMenu()
{
    if (!EditorUI::HeaderMenuButton("Overlays", "##ViewportOverlaysMenu", "What is drawn over the scene"))
        return;
    if (!ImGui::BeginPopup("##ViewportOverlaysMenu"))
        return;
    Scene::EditorOverlays& overlays = Scene::GetEditorOverlays();
    ImGui::SeparatorText("Guides");
    ImGui::Checkbox("Grid", &overlays.Grid);
    ImGui::BeginDisabled(!overlays.Grid);
    ImGui::Indent();
    ImGui::Checkbox("Axis Lines", &overlays.GridAxes);
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Opacity", &overlays.GridOpacity, 0.05f, 1.0f, "%.2f");
    ImGui::Unindent();
    ImGui::EndDisabled();
    ImGui::Checkbox("Light Icons", &overlays.LightIcons);
    ImGui::Checkbox("Game UI", &m_ShowGameUI);

    ImGui::SeparatorText("Viewport");
    ImGui::Checkbox("Navigation Gizmo", &m_ShowNavigationGizmo);
    ImGui::Checkbox("Tool Info", &m_ShowToolBadges);
    ImGui::EndPopup();
}

void Editor::DrawViewportShading()
{
    using Mode = Renderer3D::ViewMode;
    struct Button
    {
        Mode Value;
        EditorUI::ShadingIcon Icon;
        const char* Id;
        const char* Tooltip;
    };
    const bool wireframe = Renderer3D::SupportsWireframe();
    const Button buttons[] = {
        {Mode::Wireframe, EditorUI::ShadingIcon::Wireframe, "##ShadeWire",
         wireframe ? "Wireframe: triangle edges only (Shift+Z)"
                   : "Wireframe: this GPU cannot draw lines, so it shows filled"},
        {Mode::Solid, EditorUI::ShadingIcon::Solid, "##ShadeSolid", "Solid: clay under a studio light, no materials"},
        {Mode::Unlit, EditorUI::ShadingIcon::Unlit, "##ShadeUnlit", "Unlit: base colour and textures, no lighting"},
        {Mode::Lit, EditorUI::ShadingIcon::Lit, "##ShadeLit", "Lit: materials and scene lights"},
        {Mode::Normals, EditorUI::ShadingIcon::Normals, "##ShadeNormals", "Normals: world-space normal as colour"},
    };
    bool first = true;
    for (const Button& button : buttons)
    {
        if (!first)
            ImGui::SameLine(0.0f, 2.0f);
        first = false;
        if (EditorUI::ShadingButton(button.Id, button.Icon, m_ViewMode == button.Value, button.Tooltip,
                                    ImGui::GetFrameHeight()))
            SetViewMode(button.Value);
    }
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
    if (m_SceneHireacyPanel.GetSelectedEntity() && m_SceneHireacyPanel.GetSelectedEntity().HasComponent<TagComponent>())
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
    const char* shadingModes[] = {"Lit", "Unlit", "Solid", "Normals", "Wireframe", "Overdraw", "Light Complexity"};
    int shading = static_cast<int>(m_ViewMode);
    if (ImGui::Combo("Viewport Shading", &shading, shadingModes, IM_ARRAYSIZE(shadingModes)))
        SetViewMode(static_cast<Renderer3D::ViewMode>(shading));
    Scene::EditorOverlays& overlays = Scene::GetEditorOverlays();
    ImGui::Checkbox("Show Grid", &overlays.Grid);
    ImGui::Checkbox("Show Light Icons", &overlays.LightIcons);
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

    EditorUI::SectionHeader("Display");
    m_DisplaySettings.Draw(Application::Get().GetWindow());
    if (m_SceneWidth > 0 && m_SceneHeight > 0)
        ImGui::TextDisabled("Viewport renders at %u x %u", m_SceneWidth, m_SceneHeight);

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
        // Closed after the rest of the section: the rows below still read the project this frame.
        const bool closeProject = ImGui::Button("Close Project");

        // The window an exported game opens (Project.uheproject, Window block).
        EditorUI::SectionHeader("Game Window");
        ProjectWindowSettings& window = m_Project->Window;
        bool changed = false;
        char title[128];
        std::snprintf(title, sizeof(title), "%s", window.Title.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##GameTitle", m_Project->Name.c_str(), title, sizeof(title)))
            window.Title = title;
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Window title; empty uses the project name");

        int size[2] = {static_cast<int>(window.Width), static_cast<int>(window.Height)};
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragInt2("##GameSize", size, 1.0f, ProjectWindowSettings::kMinSize, ProjectWindowSettings::kMaxSize,
                            "%d px", ImGuiSliderFlags_AlwaysClamp))
        {
            window.Width = static_cast<u32>(size[0]);
            window.Height = static_cast<u32>(size[1]);
        }
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Windowed size (width, height)");

        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("Mode##GameMode", WindowModeName(window.Mode)))
        {
            for (const WindowMode mode : {WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen})
            {
                if (ImGui::Selectable(WindowModeName(mode), mode == window.Mode))
                {
                    window.Mode = mode;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        changed |= ImGui::Checkbox("VSync##GameVSync", &window.VSync);

        if (changed)
        {
            std::string error;
            if (!Project::Save(*m_Project, &error))
                UHE_CORE_WARN("Could not save the project manifest: {0}", error);
        }
        if (closeProject)
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
        case 7:
        {
            // The panel shines along local -Z; tilted to face down from 3 m it lights the ground as soon as it exists.
            entity.AddComponent<AreaLightComponent>();
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = glm::vec3(0.0f, 3.0f, 0.0f);
            transform.Rotation.x = glm::radians(-90.0f);
            break;
        }
        // Water stays at the origin, where CreateEntity puts it.
        case 8:
            entity.AddComponent<WaterComponent>();
            break;
        case 9:
            // Raised off the ground so its viewport gizmo is not buried in the floor.
            entity.AddComponent<WindComponent>();
            entity.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 2.0f, 0.0f);
            break;
        // Terrain is centred on the origin; its lowest point is the entity's height.
        case 10:
            entity.AddComponent<TerrainComponent>();
            break;
        default: break;
    }
    m_SceneHireacyPanel.SetSelectedEntity(entity);
}

void Editor::CreateUIElement(int kind)
{
    if (!m_ActiveScene || m_SceneState != SceneState::Edit)
        return;
    static constexpr const char* kNames[] = {"Panel", "Text", "Button", "Progress Bar", "Slider"};
    kind = std::clamp(kind, 0, 4);
    Entity selected = m_SceneHireacyPanel.GetSelectedEntity();
    Entity entity = selected && selected.HasComponent<UIRectComponent>()
                        ? m_ActiveScene->CreateChildEntity(selected, kNames[kind])
                        : m_ActiveScene->CreateEntity(kNames[kind]);
    auto& rect = entity.AddComponent<UIRectComponent>();
    switch (kind)
    {
        case 0:
            rect.Size = {320.0f, 200.0f};
            entity.AddComponent<UIImageComponent>();
            break;
        case 1:
            rect.Size = {240.0f, 40.0f};
            entity.AddComponent<UITextComponent>();
            break;
        case 2:
        {
            rect.Size = {160.0f, 44.0f};
            auto& image = entity.AddComponent<UIImageComponent>();
            image.CornerRadius = 8.0f;
            entity.AddComponent<UIButtonComponent>();
            auto& text = entity.AddComponent<UITextComponent>();
            text.Text = "Button";
            text.FontSize = 20.0f;
            break;
        }
        default:
        {
            rect.Size = {240.0f, 18.0f};
            auto& bar = entity.AddComponent<UIProgressBarComponent>();
            bar.Value = 0.6f;
            bar.Interactable = kind == 4;
            break;
        }
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
