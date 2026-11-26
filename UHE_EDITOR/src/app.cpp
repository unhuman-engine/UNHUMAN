
// clang-format off: EntryPoint.h is not self-contained, it needs UHE.h first.
#include "UHE.h"
#include <UHE/Core/EntryPoint.h>
// clang-format on
#include <imgui/imgui.h>
#include "Editor.h"
#include "EditorDebugSettings.h"

namespace UHE
{

	class UHEEditor : public Application
	{
	public:
		UHEEditor()
			: Application("UNHUMAN ENGINE")
		{
			//PushLayer(new ExampleLayer());	
			PushLayer(new Editor());
		}

		~UHEEditor() override
		{
			
		}
	};


	Application* CreateApplication()
	{
        // Before the Application: its constructor creates the Vulkan instance,
        // which fixes sync validation and RenderDoc's hooks for the run.
        EditorDebugSettings::Load();
        return new UHEEditor();
    }

}