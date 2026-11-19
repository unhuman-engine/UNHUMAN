#pragma once
#include "Core.h"
#include "Window.h"
#include "UHE/Core/LayerStack.h"

#include "UHE/Events/ApplicationEvent.h"
#include "UHE/ImGui/ImGuiLayer.h"
#include "UHE/Core/Timestep.h"

namespace UHE {


	class UHE_API Application
	{
	public:
		Application(const std::string& name = "UHE APP");
		virtual ~Application();

		void Run();

		void OnEvent(Event& e);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* layer);

		ImGuiLayer* GetImguiLayer() { return m_ImGuiLayer; };

		void Close();
		//bool OnWindowResize(WindowResizeEvent& e);

		inline Window& GetWindow() { return *m_Window; }
		inline static Application& Get() { return *s_instance; }
	private:
		bool OnWindowClose(WindowCloseEvent& e);
		bool OnWindowResize(WindowResizeEvent& e);

		
		std::unique_ptr<Window> m_Window;
		ImGuiLayer* m_ImGuiLayer;
		bool m_Running = true;
		bool m_Minimized = false;

		LayerStack m_LayerStack;
        double m_LastFrameTime = 0.0; ///< @brief glfwGetTime() at the last frame.
    private:
		
		

		
		static Application* s_instance;
	};

	//T0 be define in class
	Application* CreateApplication();

}
