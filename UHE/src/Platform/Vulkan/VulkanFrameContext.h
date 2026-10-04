#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/VulkanCommandBuffer.h"
#include "Platform/Vulkan/VulkanCommandPool.h"
#include "UHE/RHI/DeletionQueue.h"

namespace UHE::RHI::VULKAN
{

/**
 * \brief Per-frame command buffer, pool and sync handles.
 *
 * One instance exists per frame-in-flight. The deletion queue lets resources be
 * retired safely: their teardown runs once this frame's fence/timeline has passed.
 */
class VulkanFrameContext
{
public:
    VulkanFrameContext() = default;

    void Init(vk::raii::Device& device, u32 queueFamilyIndex);

    void Cleanup();

    inline VulkanCommandBuffer& GetCommandBuffer() { return commandBuffer; }
    inline vk::raii::Semaphore& GetimageAvailableSemaphore() { return imageAvailableSemaphore; }
    inline vk::raii::Fence& GetInFlightFence() { return inFlightFence; }
    inline DeletionQueue& GetDeletionQueue() { return deletionQueue; }

private:
    VulkanCommandPool commandPool;
    VulkanCommandBuffer commandBuffer;
    vk::raii::Semaphore imageAvailableSemaphore = nullptr;
    vk::raii::Fence inFlightFence = nullptr;

    // Deferred resource destruction queue per-frame
    DeletionQueue deletionQueue;
};

} // namespace UHE::RHI::VULKAN
