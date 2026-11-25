#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "CommandPalette.h"
#include "EditorDisplaySettings.h"
#include "Notifications.h"
#include "Panel/AnimationGraphPanel.h"
#include "Panel/AudioPanel.h"
#include "Panel/ContentBrowserPanel.h"
#include "Panel/EnvironmentPanel.h"
#include "Panel/ProfilerPanel.h"
#include "Panel/ProjectLauncherPanel.h"
#include "Panel/SceneHierachyPanel.h"
#include "Panel/ScriptsPanel.h"
#include "UHE.h"
#include "UHE/Renderer/EditorCamera.h"
#include "UHE/Renderer3D/Renderer3D.h"
#include "imgui.h"

namespace UHE::RHI
{
class FGContext;
}

namespace UHE
{
class Editor : public Layer
{
public:
    Editor();
    virtual ~Editor() = default;

    void OnAttach() override;
    void OnDetach() override;

    void OnUpdate(Timestep ts) override;
    void OnEvent(Event& event) override;

    virtual void OnImGuiRender() override;
    bool OnMouseButtonPressedEvent(MouseButtonPressedEvent& e);

    void UI_Toolbar();

private:
    void OnScreenPlay();
    void OnScreenPause();
    void OnScreenStep();
    void OnSceneStop();
    bool onKeyPressed(KeyPressedEvent& e);
    void NewScene(bool notify = true);
    void OpenScene();
    void SaveSceneAs();
    void OpenScene(const std::filesystem::path& path, bool notify = true);

    // Project lifecycle: the editor has no asset root until a project is open.
    bool OpenProject(const std::filesystem::path& path);
    void SetProject(ProjectInfo info);
    void CloseProject();
    [[nodiscard]] std::filesystem::path ProjectAssetPath() const;

    // Files dropped from the OS file explorer: imported into the project, and
    // placed in the scene when they land on the viewport.
    bool OnFileDrop(WindowFileDropEvent& e);
    bool CreateEntityFromAsset(const std::filesystem::path& assetPath);

    void DrawMainMenuBar();
    void DrawEditorToolbar();
    void DrawViewportHeader();
    // The viewport header's right side, Blender-style: camera and overlay
    // menus, then the shading mode buttons.
    void DrawViewportCameraMenu();
    void DrawViewportOverlaysMenu();
    void DrawViewportShading();
    void SetViewMode(Renderer3D::ViewMode mode);
    void DrawViewportOverlay(const ImVec2& imageMin, const ImVec2& imageMax);
    void DrawAxisGizmo(ImDrawList* draw, const ImVec2& center, float radius);
    void DrawStatusBar();
    void DrawPreferencesContent();
    void DrawPreferencesModal();
    void DrawAboutModal();
    void CreateEntityFromMenu(const char* name, int componentKind);

    // ---- editor ergonomics ----
    void Notify(NotificationCenter::Kind kind, std::string title, std::string message = {});
    std::vector<CommandPalette::Command> BuildCommands();
    void FocusSelectedEntity();
    void DeleteSelectedEntity();
    void ReloadShaders();
    void ReloadScripts();

    /// @brief Adds the editor's cvars and commands to the in-app console.
    void RegisterConsoleEntries();
    /// @brief Removes them again; the console outlives the editor layer.
    void UnregisterConsoleEntries();

    // Transform undo/redo. The baseline is refreshed on selection changes;
    // a completed edit (mouse released, gizmo idle) pushes it once.
    void PollTransformUndo();
    void ResetTransformBaseline();
    void UndoTransform();
    void RedoTransform();

    // The simulation runs in OnUpdate; the scene is drawn by a frame graph
    // pass, recorded before the ImGui pass that shows it.
    void DeclareSceneGraphPass(Timestep ts, bool runtime);
    /// Seconds the viewport has rendered; animates wind and water.
    float m_SceneTime = 0.0f;
    /// @brief The colour pass's draws, for \p scene: the one the frame was declared with.
    void RecordSceneGraphPass(UHE::RHI::FGContext& context, Scene& scene, Timestep ts, bool runtime);

private:
    Entity m_Square;
    Entity m_CameraEntity;
    Entity m_SecondCamera;

    Entity m_HoverdEntity;
    bool primaryCamera = false;
    EditorCamera m_EditorCamera;
    bool m_ViewPortFocused = false;
    bool m_ViewPortHover = false;
    bool m_IsViewportFullScreen = false;

    OrthographicCameraContoroller m_CameraController;

    Ref<Scene> m_ActiveScene;
    Ref<Scene> m_EditorScene;

    glm::vec4 blueColor = {0.2f, 0.3f, 0.8f, 1.0f};
    glm::vec3 m_Transform;
    glm::vec4 redColor = {0.8f, 0.2f, 0.3f, 1.0f};

    u32 m_mapWidth, m_mapHeight;

    glm::vec2 m_ViewPortSize{0.0f}; // glm leaves vectors uninitialized: a garbage size skipped the seed in OnAttach
    glm::vec2 m_ViewPortBounds[2]{};
    int m_GizmoType = -1;
    bool m_GizmoSpaceWorld = false;
    bool m_SnapEnabled = false;

    SceneHierarchyPanel m_SceneHireacyPanel;
    AnimationGraphPanel m_AnimationGraphPanel;
    AudioPanel m_AudioPanel;
    ContentBrowserPanel m_ContentBrowserPanel;
    EnvironmentPanel m_EnvironmentPanel; ///< the active scene's RenderSettings, a tab beside Properties
    ProfilerPanel m_ProfilerPanel;
    ScriptsPanel m_ScriptsPanel;
    ProjectLauncherPanel m_ProjectLauncher;
    CommandPalette m_CommandPalette;
    NotificationCenter m_Notifications;
    EditorDisplaySettings m_DisplaySettings;
    std::optional<ProjectInfo> m_Project;
    bool m_ShowLauncher = true;
    // Scene targets: created by the frame graph at the viewport's size times
    // the render scale, and exported under these names for the viewport image
    // and picking.
    static constexpr const char* kSceneColor = "Editor.Scene.Color";
    static constexpr const char* kSceneEntity = "Editor.Scene.Entity";
    static constexpr u32 kMaxSceneSize = 8192;
    u32 m_SceneWidth = 0;
    u32 m_SceneHeight = 0;
    Ref<Texture2D> m_IconPlay;
    Ref<Texture2D> m_IconPause;
    Ref<Texture2D> m_IconStop;
    uint32_t m_FramesSinceResize = 0;

    std::filesystem::path m_CurrentScenePath;
    bool m_StepRequested = false;

    bool m_ShowHierarchy = true;
    bool m_ShowInspector = true;
    bool m_ShowEnvironment = true;
    bool m_ShowContentBrowser = true;
    bool m_ShowConsole = true;
    bool m_ShowProfiler = true;
    bool m_ShowScripts = true;
    bool m_ShowAnimationGraph = true;
    bool m_ShowAudio = true;
    bool m_ShowSettings = true;
    bool m_ShowViewport = true;
    bool m_ShowPreferences = false;
    bool m_ShowAbout = false;
    bool m_ResetLayout = false;
    bool m_ShowViewportOverlay = true;
    // Viewport display. The shading mode applies to the scene pass; the grid
    // and light icons live in Scene::GetEditorOverlays().
    Renderer3D::ViewMode m_ViewMode = Renderer3D::ViewMode::Lit;
    Renderer3D::ViewMode m_ViewModeBeforeWireframe = Renderer3D::ViewMode::Lit; ///< Shift+Z goes back to it
    bool m_ShowNavigationGizmo = true;
    bool m_ShowToolBadges = true;
    float m_SnapValue = 0.5f;

    // Transform undo/redo. The editor records one entry per completed edit
    // (gizmo drag released, inspector slider released, text field committed),
    // so Ctrl+Z steps back through changes instead of frame by frame.
    struct TransformSnapshot
    {
        u64 EntityID = 0;
        glm::vec3 Translation{0.0f};
        glm::vec3 Rotation{0.0f};
        glm::vec3 Scale{1.0f};
    };
    std::vector<TransformSnapshot> m_UndoStack;
    std::vector<TransformSnapshot> m_RedoStack;
    TransformSnapshot m_TransformBaseline;
    u64 m_TransformBaselineEntity = 0;
    float m_SecondsSinceTransformPush = 1000.0f;

    enum class SceneState
    {
        Edit = 0,
        Play = 1,
        Paused = 2,
    };

    SceneState m_SceneState = SceneState::Edit;
};
} // namespace UHE
