#pragma once
#include <GLFW/glfw3.h>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHISwapChain.h"

namespace UHE::RHI::VULKAN
{
/**
 * \brief Vulkan implementation of RHISwapChain built from a GLFW window surface.
 *
 * \note Owns the swapchain, its images/views and the present mode negotiation.
 * \see  https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainKHR.html
 */
class UHE_API VulkanSwapChain final : public RHISwapChain
{
public:
    VulkanSwapChain() = default;
    VulkanSwapChain(const VulkanSwapChain&) = delete;
    VulkanSwapChain& operator=(const VulkanSwapChain&) = delete;

    void createSwapChain(vk::raii::Device& device, vk::raii::PhysicalDevice& physicalDevice,
                         vk::raii::SurfaceKHR& surface, GLFWwindow* window);
    void cleanupSwapChain();

    void AcquireNextImage() override;
    void Present() override;
    void ResizeSwapchain(u32 width, u32 height) override;
    [[nodiscard]] TextureHandle GetSwapchainImage() override;
    [[nodiscard]] TextureFormat GetSwapchainFormat() override;

    [[nodiscard]] vk::raii::SwapchainKHR& GetSwapchain() { return swapChain; }
    [[nodiscard]] const std::vector<vk::Image>& GetImages() const { return swapChainImages; }
    [[nodiscard]] vk::raii::ImageView& GetImageView(u32 index) { return swapChainImageViews[index]; }
    [[nodiscard]] const vk::Extent2D& GetExtent() const { return swapChainExtent; }
    [[nodiscard]] vk::SurfaceFormatKHR& GetSurfaceFormat() { return swapChainSurfaceFormat; }

private:
    vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR& capabilities, GLFWwindow* window);
    u32 chooseSwapMinImageCount(vk::SurfaceCapabilitiesKHR const& surfaceCapablities);
    vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats);
    vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR>& availablePresentModes);

private:
    vk::raii::SwapchainKHR swapChain = nullptr;
    std::vector<vk::Image> swapChainImages;
    vk::SurfaceFormatKHR swapChainSurfaceFormat;
    vk::Extent2D swapChainExtent;
    std::vector<vk::raii::ImageView> swapChainImageViews;
    vk::raii::Sampler textureSampler = nullptr;
    vk::raii::ImageView textureImageView = nullptr;
};

} // namespace UHE::RHI::VULKAN
