#include "uhepch.h"
#include "VulkanSwapChain.h"

namespace UHE::RHI::VULKAN
{
void VulkanSwapChain::createSwapChain(vk::raii::Device& device, vk::raii::PhysicalDevice& physicalDevice,
                                      vk::raii::SurfaceKHR& surface, GLFWwindow* window)
{
    auto SurfaceCapabilities = physicalDevice.getSurfaceCapabilitiesKHR(*surface);
    swapChainExtent = chooseSwapExtent(SurfaceCapabilities, window);
    swapChainSurfaceFormat = chooseSwapSurfaceFormat(physicalDevice.getSurfaceFormatsKHR(*surface));
    auto SurfaceFormats = physicalDevice.getSurfaceFormatsKHR(*surface);

    // Transfer-source lets RequestFrameCapture copy a presented image back
    // (golden-image tests). Only asked for where the surface offers it.
    vk::ImageUsageFlags imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
    supportsTransferSrc =
        static_cast<bool>(SurfaceCapabilities.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferSrc);
    if (supportsTransferSrc)
        imageUsage |= vk::ImageUsageFlagBits::eTransferSrc;

    vk::SwapchainCreateInfoKHR swapChainCreateInfo{
        .flags = {},
        .surface = *surface,
        .minImageCount = chooseSwapMinImageCount(SurfaceCapabilities),
        .imageFormat = swapChainSurfaceFormat.format,
        .imageColorSpace = swapChainSurfaceFormat.colorSpace,
        .imageExtent = swapChainExtent,
        .imageArrayLayers = 1,
        .imageUsage = imageUsage,
        .imageSharingMode = vk::SharingMode::eExclusive,
        .queueFamilyIndexCount = {},
        .pQueueFamilyIndices = {},
        .preTransform = SurfaceCapabilities.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = chooseSwapPresentMode(physicalDevice.getSurfacePresentModesKHR(*surface)),
        .clipped = VK_TRUE,
        .oldSwapchain = {}};

    swapChain = vk::raii::SwapchainKHR(device, swapChainCreateInfo);
    swapChainImages = swapChain.getImages();

    swapChainImageViews.clear();
    swapChainImageViews.reserve(swapChainImages.size());
    for (auto image : swapChainImages)
    {
        vk::ImageViewCreateInfo createInfo{
            .flags = {},
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = swapChainSurfaceFormat.format,
            .components = {
                .r = vk::ComponentSwizzle::eIdentity,
                .g = vk::ComponentSwizzle::eIdentity,
                .b = vk::ComponentSwizzle::eIdentity,
                .a = vk::ComponentSwizzle::eIdentity
            },
            .subresourceRange = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        swapChainImageViews.emplace_back(device, createInfo);
    }
}

vk::Extent2D VulkanSwapChain::chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities, GLFWwindow* window)
{
    if (capabilities.currentExtent.width != 0xFFFFFFFF)
    {
        return capabilities.currentExtent;
    }
    i32 width, height;

    glfwGetFramebufferSize(window, &width, &height);

    return vk::Extent2D{
        std::clamp(static_cast<uint32_t>(width), capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        std::clamp(static_cast<uint32_t>(height), capabilities.minImageExtent.height,
                   capabilities.maxImageExtent.height)};
}

u32 VulkanSwapChain::chooseSwapMinImageCount(vk::SurfaceCapabilitiesKHR const& surfaceCapablities)
{
    auto minImageCount = std::max(3u, surfaceCapablities.minImageCount);
    if ((0 < surfaceCapablities.maxImageCount) && (minImageCount > surfaceCapablities.maxImageCount))
    {
        minImageCount = surfaceCapablities.maxImageCount;
    }
    return minImageCount;
}

vk::PresentModeKHR VulkanSwapChain::chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& availablePresentModes)
{
    assert(std::ranges::any_of(availablePresentModes,
                               [](auto presentMode) { return presentMode == vk::PresentModeKHR::eFifo; }));

    return std::ranges::any_of(availablePresentModes,
                               [](const vk::PresentModeKHR value) { return vk::PresentModeKHR::eMailbox == value; })
               ? vk::PresentModeKHR::eMailbox
               : vk::PresentModeKHR::eFifo;
}

vk::SurfaceFormatKHR VulkanSwapChain::chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats)
{
    assert(!availableFormats.empty());
    const auto formatIt = std::find_if(availableFormats.begin(), availableFormats.end(),
                                       [](const auto& availableFormat)
                                       {
                                           return availableFormat.format == vk::Format::eB8G8R8A8Srgb &&
                                                  availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
                                       });
    return formatIt != availableFormats.end() ? *formatIt : availableFormats[0];
}

void VulkanSwapChain::cleanupSwapChain()
{
    swapChainImageViews.clear();
    if (swapChain != nullptr)
    {
        swapChain = nullptr;
    }
}

void VulkanSwapChain::AcquireNextImage() {}

void VulkanSwapChain::Present() {}

void VulkanSwapChain::ResizeSwapchain(u32 width, u32 height) {}

TextureHandle VulkanSwapChain::GetSwapchainImage() { return nullptr; }

TextureFormat VulkanSwapChain::GetSwapchainFormat() { return TextureFormat::BGRA8_SRGB; }
} // namespace UHE::RHI::VULKAN
