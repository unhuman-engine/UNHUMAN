#include <UHE.h>
#include <UHE/Core/EntryPoint.h>
#include "RuntimeLayer.h"

/// @brief The game runtime: one layer that plays the project's start scene.
class RuntimeApp : public UHE::Application
{
public:
    /// The window opens at the project's title and size straight away, rather
    /// than at the default size and then resizing.
    explicit RuntimeApp(std::optional<UHE::ProjectInfo> project)
        : Application("UHE Runtime", WindowProps(project ? &*project : nullptr))
    {
        PushLayer(new RuntimeLayer(std::move(project)));
    }

private:
    static UHE::WindowProps WindowProps(const UHE::ProjectInfo* project)
    {
        const UHE::ProjectWindowSettings window = RuntimeLayer::ResolveWindow(project);
        return UHE::WindowProps(window.Title, window.Width, window.Height);
    }
};

UHE::Application* UHE::CreateApplication()
{
    return new RuntimeApp(RuntimeLayer::FindProject());
}
