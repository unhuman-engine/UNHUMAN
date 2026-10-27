#include "uhepch.h"

#include "Application.h"

#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/Renderer2D.h"
#include "UHE/Renderer3D/Renderer3D.h"

#include "UHE/Core/Log.h"
#include "UHE/Physics/PhysicsSystem3D.h"
#include "input.h"
#include "UHE/Audio/AudioEngine.h"

#include <GLFW/glfw3.h>

namespace UHE{

#define BIND_EVENT_FN(x) std::bind(&Application::x,this,std::placeholders::_1)

	Application* Application::s_instance = nullptr;

	

	Application::Application( const std::string& name)
		
	{   
		UHE_PROFILE_FUNCTION();
		UHE_CORE_ASSERT(!s_instance, "Application already exists!");
		
		s_instance = this;
		m_Window = std::unique_ptr<Window>(Window::Create(WindowProps(name)));
		m_Window->SetEventCallback(BIND_EVENT_FN(OnEvent));
		m_Window->SetVSync(false);
		
		Renderer::Init();
		Renderer2D::Init();
		Renderer3D::Init();
		Physics::PhysicsSystem3D::Init();
		Audio::AudioEngine::Init();

		m_ImGuiLayer = ImGuiLayer::Create();
		PushOverlay(m_ImGuiLayer);		
	}
	 
	Application::~Application()
	{
		Renderer::GetDevice().WaitIdle();
		Renderer::GetDevice().ResetCommandBuffers();
		m_LayerStack.Clear();
		Audio::AudioEngine::Shutdown();
		Physics::PhysicsSystem3D::Shutdown();
		Renderer3D::Shutdown();
		Renderer2D::Shutdown();
		Renderer::Shutdown();
	}

	void Application::PushLayer(Layer* layer)
	{
		m_LayerStack.PushLayer(layer);
		layer->OnAttach();
	}

	void Application::PushOverlay(Layer* layer)
	{
		m_LayerStack.PushOverlay(layer);
		layer->OnAttach();
	}

	void Application::Close()
	{
		m_Running = false;
	}

	//bool Application::OnWindowResize(WindowResizeEvent& e)
	//{
	//	// Update the OpenGL viewport immediately
	//	glViewport(0, 0, (int)e.GetWidth(), (int)e.GetHeight());
	//	UHE_CORE_INFO("Window resized: {0}, {1}", e.GetWidth(), e.GetHeight());
	//	// return false so other layers may also handle the event if needed
	//	return false;
	//}
	
	void Application::OnEvent(Event& e)
	{
		EventDispatcher dispatcher(e);
		dispatcher.Dispatch<WindowCloseEvent>(BIND_EVENT_FN(OnWindowClose));
		dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FN(OnWindowResize)); // <-- add dispatch

		//UHE_CORE_TRACE("{0}", e.ToString());

		
		for (auto it = m_LayerStack.end(); it != m_LayerStack.begin();)
		{
			(*--it)->OnEvent(e);
			if (e.Handled)
				break;
		}
	};
	
	/*OnWindowResize(WindowResizeEvent& e);*/
	
	void Application::Run() {
    // Dispatch an initial resize event to set up the viewport and camera
    {
			
        int width = m_Window->GetWidth();
        int height = m_Window->GetHeight();
        WindowResizeEvent e(width, height);
        OnEvent(e);
    }

	while (m_Running)
	{   
		UHE::Profiler::BeginFrame();
		{
			UHE_PROFILE_SCOPE("Frame");
			{
				UHE_PROFILE_SCOPE("application");
				float time = (float)glfwGetTime();

				Timestep timestep = time - m_LastFrameTime;
				m_LastFrameTime = time;

				if (timestep > 0.25f)
					timestep = 0.25f;

				if (!m_Minimized) {
					Audio::AudioEngine::Update();
					
					Renderer::GetDevice().Begin();
					// §9.1.4 resize policy: Begin may recreate the swapchain and skip
					// the frame — layers must not record GPU work this iteration.
					const bool frameSkipped = Renderer::GetDevice().FrameSkipped();

					if (!frameSkipped)
					{
						for (Layer* layer : m_LayerStack)
							layer->OnUpdate(timestep);
					}

					m_ImGuiLayer->Begin();
					for (Layer* layer : m_LayerStack)
						layer->OnImGuiRender();
					m_ImGuiLayer->End();

					Renderer::GetDevice().End();
				}

				m_Window->OnUpdate();
				
				{
					UHE_PROFILE_SCOPE("TracyDummyTestWork");
					float result = 0.0f;
					for (int i = 0; i < 100000; i++) {
						result += std::sin((float)i) * std::cos((float)i);
					}
					if (result == 0.12345f) { // Prevent compiler optimization
						UHE_CORE_INFO("Dummy work result: {}", result);
					}
				}
			}
			UHE_PROFILE_FRAMEMARK;
		}
		UHE::Profiler::EndFrame();
	}

	};
	bool Application::OnWindowClose(WindowCloseEvent& e) 
	{   
		m_Running = false;
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& e)
	{
		if(e.GetWidth() == 0 || e.GetHeight() == 0)
		{
			m_Minimized = true;
			return false;
		}
		m_Minimized = false;
		Renderer::OnWindowResize(e.GetWidth(), e.GetHeight());
		return false;
	}
}
