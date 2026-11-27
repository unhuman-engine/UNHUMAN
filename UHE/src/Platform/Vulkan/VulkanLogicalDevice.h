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
    /// The dedicated compute queue, or the graphics one on a GPU without a compute-only family.
    [[nodiscard]] vk::raii::Queue& getComputeQueue()
    {
        return HasDedicatedCompute() ? m_computeQueue : m_graphicsQueue;
    }
    /// The dedicated transfer queue, or the graphics one on a GPU without a transfer-only family.
    [[nodiscard]] vk::raii::Queue& getTransferQueue()
    {
        return HasDedicatedTransfer() ? m_transferQueue : m_graphicsQueue;
    }
    [[nodiscard]] u32 getComputeQueueFamilyIndex() const
    {
        return HasDedicatedCompute() ? m_computeQueueFamilyIndex : m_graphicsQueueFamilyIndex;
    }
    [[nodiscard]] u32 getTransferQueueFamilyIndex() const
    {
        return HasDedicatedTransfer() ? m_transferQueueFamilyIndex : m_graphicsQueueFamilyIndex;
    }
    [[nodiscard]] bool HasDedicatedCompute() const { return m_computeQueueFamilyIndex != static_cast<u32>(-1); }
    [[nodiscard]] bool HasDedicatedTransfer() const { return m_transferQueueFamilyIndex != static_cast<u32>(-1); }
    [[nodiscard]] vk::raii::SurfaceKHR& getSurface() { return surface; }

private:
    u32 m_graphicsQueueFamilyIndex{0};
    vk::raii::Device m_logicalDevice = nullptr;
    vk::raii::Queue m_graphicsQueue = nullptr;
    u32 m_computeQueueFamilyIndex = static_cast<u32>(-1);  ///< compute without graphics; -1: none
    u32 m_transferQueueFamilyIndex = static_cast<u32>(-1); ///< transfer only; -1: none
    vk::raii::Queue m_computeQueue = nullptr;
    vk::raii::Queue m_transferQueue = nullptr;
    vk::raii::SurfaceKHR surface = nullptr;
    VmaAllocator m_allocator = nullptr;
};
} // namespace UHE::RHI::VULKAN
