#pragma once
#include <vulkan/vulkan_raii.hpp>

namespace UHE::RHI::VULKAN
{

/**
 * \brief Owns a VkCommandPool; command buffers allocated from it share a queue family.
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkCommandPool.html
 */
class VulkanCommandPool
{
public:
    VulkanCommandPool() = default;
    ~VulkanCommandPool() = default;
    VulkanCommandPool(const VulkanCommandPool&) = delete;
    VulkanCommandPool& operator=(const VulkanCommandPool&) = delete;

    void Init(vk::raii::Device& device, u32 queueFamilyIndex, vk::CommandPoolCreateFlags flags = {});
    void CleanUp();
    /// Recycles every command buffer allocated from this pool in one call.
    void Reset(vk::CommandPoolResetFlags flags = {});

    [[nodiscard]] const vk::raii::CommandPool& GetHandle() const { return m_CommandPool; }
    [[nodiscard]] vk::raii::CommandPool& GetHandle() { return m_CommandPool; }

private:
    vk::raii::CommandPool m_CommandPool{nullptr};
};

} // namespace UHE::RHI::VULKAN
