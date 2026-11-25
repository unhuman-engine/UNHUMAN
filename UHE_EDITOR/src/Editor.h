#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "CommandPalette.h"
#include "Notifications.h"
#include "Panel/AnimationGraphPanel.h"
#include "Panel/AudioPanel.h"
#include "Panel/ContentBrowserPanel.h"
#include "Panel/ProfilerPanel.h"
#include "Panel/ProjectLauncherPanel.h"
#include "Panel/SceneHierachyPanel.h"
#include "Panel/ScriptsPanel.h"
#include "UHE.h"
#include "UHE/Renderer/EditorCamera.h"
#include "UHE/Renderer/Framebuffer.h"
#include "imgui.h"

namespace UHE::RHI::VULKAN
{
class RGPassContext;
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

    // Render graph path: the simulation runs in OnUpdate and the scene draw
    // stream is recorded by a deferred graph pass before the ImGui pass.
    void DeclareSceneGraphPass(Timestep ts, bool runtime);
    void RecordSceneGraphPass(UHE::RHI::VULKAN::RGPassContext& context, Timestep ts, bool runtime);

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

    glm::vec2 m_ViewPortSize;
    glm::vec2 m_ViewPortBounds[2];
    int m_GizmoType = -1;
    bool m_GizmoSpaceWorld = false;
    bool m_SnapEnabled = false;

    SceneHierarchyPanel m_SceneHireacyPanel;
    AnimationGraphPanel m_AnimationGraphPanel;
    AudioPanel m_AudioPanel;
    ContentBrowserPanel m_ContentBrowserPanel;
    ProfilerPanel m_ProfilerPanel;
    ScriptsPanel m_ScriptsPanel;
    ProjectLauncherPanel m_ProjectLauncher;
    CommandPalette m_CommandPalette;
    NotificationCenter m_Notifications;
    std::optional<ProjectInfo> m_Project;
    bool m_ShowLauncher = true;
    Ref<Framebuffer> m_Framebuffer;
    Ref<Texture2D> m_IconPlay;
    Ref<Texture2D> m_IconPause;
    Ref<Texture2D> m_IconStop;
    uint32_t m_FramesSinceResize = 0;

    std::filesystem::path m_CurrentScenePath;
    bool m_StepRequested = false;

    bool m_ShowHierarchy = true;
    bool m_ShowInspector = true;
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

    enum class RenderPath
    {
        Immediate = 0,
        RenderGraph = 1,
    };

    SceneState m_SceneState = SceneState::Edit;
    RenderPath m_RenderPath = RenderPath::RenderGraph;
};
} // namespace UHE
