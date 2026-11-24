#include "uhepch.h"
#include "Renderer.h"
#include "Renderer2D.h"
#include "UHE/Core/Application.h"

namespace UHE {

std::unique_ptr<RHI::RHIDevice> Renderer::s_Device = nullptr;

void Renderer::Init() {
    auto& window = Application::Get().GetWindow();

    RHI::SwapchainDesc swapDesc{};
    swapDesc.nativeWindow = window.GetNativeWindow();
    swapDesc.width = window.GetWidth();
    swapDesc.height = window.GetHeight();
    swapDesc.vsync = false;

    s_Device = RHI::RHIDevice::Create(RHI::Backend::Vulkan, swapDesc);
    UHE_CORE_INFO("Renderer initialized with Vulkan backend");
}

void Renderer::Shutdown() {
    s_Device.reset();
    UHE_CORE_INFO("Renderer shut down");
}

void Renderer::OnWindowResize(u32 width, u32 height) {
    // The device sizes the swapchain from the surface itself; it only needs to
    // know a resize happened. A zero size is a minimize, which the application
    // handles by not rendering.
    if (s_Device && width > 0 && height > 0)
        s_Device->OnFramebufferResized();
}

RHI::RHIDevice& Renderer::GetDevice() {
    UHE_CORE_ASSERT(s_Device, "Renderer not initialized!");
    return *s_Device;
}

} // namespace UHE
