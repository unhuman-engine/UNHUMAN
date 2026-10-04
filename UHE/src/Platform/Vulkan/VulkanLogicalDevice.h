#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>

namespace UHE::RHI::VULKAN
{
class VulkanPhysicalDevice;
class VulkanInstance;
class VulkanExtensionCheck;

/**
 * \brief Owns the logical device, its graphics queue, the surface and the VMA allocator.
 *
 * Queue family selection and device capability tiers are negotiated here, so the rest
 * of the backend can assume a single graphics queue and a valid allocator.
 *
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkDevice.html
 */
class VulkanLogicalDevice
{
public:
    VulkanLogicalDevice() = default;
    ~VulkanLogicalDevice() = default;
    VulkanLogicalDevice(const VulkanLogicalDevice&) = delete;
    VulkanLogicalDevice& operator=(const VulkanLogicalDevice&) = delete;

    void initialize(VulkanPhysicalDevice& physicalDevice, VkSurfaceKHR surface, VulkanInstance& instance,
                    VulkanExtensionCheck& CheckExtens);
    void CreateSurface(VulkanInstance& instance, GLFWwindow* window);

    void cleanup();

    [[nodiscard]] VmaAllocator& getAllocator() { return m_allocator; }

    [[nodiscard]] vk::raii::Device& getLogicalDevice() { return m_logicalDevice; }
    [[nodiscard]] u32 getGraphicsQueueFamilyIndex() const { return m_graphicsQueueFamilyIndex; }
    [[nodiscard]] vk::raii::Queue& getGraphicsQueue() { return m_graphicsQueue; }
    [[nodiscard]] vk::raii::SurfaceKHR& getSurface() { return surface; }

private:
    u32 m_graphicsQueueFamilyIndex{0};
    vk::raii::Device m_logicalDevice = nullptr;
    vk::raii::Queue m_graphicsQueue = nullptr;
    vk::raii::SurfaceKHR surface = nullptr;
    VmaAllocator m_allocator = nullptr;

    // \todo Dead member — device-extension selection is done in initialize(). Either
    //       wire it in or delete it (tracked in docs/ROADMAP.md).
    std::vector<const char*> requiredDeviceExtension = {
        vk::KHRSwapchainExtensionName,           vk::KHRSpirv14ExtensionName,
        vk::KHRSynchronization2ExtensionName,    vk::KHRCreateRenderpass2ExtensionName,
        vk::KHRShaderFloatControlsExtensionName, vk::KHRDynamicRenderingExtensionName};
};
} // namespace UHE::RHI::VULKAN
