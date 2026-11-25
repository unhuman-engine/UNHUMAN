#include <UHE.h>
#include <UHE/Core/EntryPoint.h>
#include "RuntimeLayer.h"

/// @brief The game runtime: one layer that plays the project's start scene.
class RuntimeApp : public UHE::Application
{
public:
    RuntimeApp() { PushLayer(new RuntimeLayer()); }
};

UHE::Application* UHE::CreateApplication()
{
    return new RuntimeApp();
}
