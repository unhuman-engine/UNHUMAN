#pragma once
#include "UHE.h"

#include "Panel/SceneHierachyPanel.h"
#include "Panel/ProfilerPanel.h"
#include "UHE/Renderer/EditorCamera.h"
#include "Panel/ContentBrowserPanel.h"
#include "UHE/Renderer/Framebuffer.h"

#include <filesystem>

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
		void NewScene();
		void OpenScene();
		void SaveSceneAs();
		void OpenScene(const std::filesystem::path& path);

		void DrawMainMenuBar();
		void DrawEditorToolbar();
		void DrawViewportHeader();
		void DrawStatusBar();
		void DrawPreferencesContent();
		void DrawPreferencesModal();
		void DrawAboutModal();
		void CreateEntityFromMenu(const char* name, int componentKind);

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

		glm::vec4 blueColor = { 0.2f, 0.3f, 0.8f, 1.0f };
		glm::vec3 m_Transform;
		glm::vec4 redColor = { 0.8f, 0.2f, 0.3f, 1.0f };

		u32 m_mapWidth, m_mapHeight;

		glm::vec2 m_ViewPortSize;
		glm::vec2 m_ViewPortBounds[2];
		int m_GizmoType = -1;
		bool m_GizmoSpaceWorld = false;
		bool m_SnapEnabled = false;

		SceneHierarchyPanel m_SceneHireacyPanel;
		ContentBrowserPanel m_ContentBrowserPanel;
		ProfilerPanel m_ProfilerPanel;
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
		bool m_ShowSettings = true;
		bool m_ShowViewport = true;
		bool m_ShowPreferences = false;
		bool m_ShowAbout = false;
		bool m_ResetLayout = false;
	    
		enum class SceneState
		{
			Edit = 0, Play = 1, Paused = 2,
		};

		enum class RenderPath
		{
			Immediate = 0,
			RenderGraph = 1,
		};

		SceneState m_SceneState = SceneState::Edit;
		RenderPath m_RenderPath = RenderPath::RenderGraph;
	};
}
