#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "UHE/Core/Core.h"

namespace UHE::RHI::VULKAN
{
class VulkanContext;

/**
 * \brief Host-side fence for "has the GPU finished?" synchronisation.
 *
 * A fence is signalled by a queue submission and waited on by the CPU — the mirror
 * image of a semaphore, which synchronises between GPU queues instead. The frame
 * loop typically waits on one fence per frame in flight.
 *
 * \note This overlaps with VulkanBinaryFence; the two should be consolidated.
 * \see  https://docs.vulkan.org/refpages/latest/refpages/source/VkFence.html
 */
class VulkanFence
{
public:
    VulkanFence() = default;
    ~VulkanFence() = default;
    VulkanFence(const VulkanFence&) = delete;
    VulkanFence& operator=(const VulkanFence&) = delete;

    /// Creates the fence. Pass \p signaled = true for the first frame so the CPU
    /// does not block on a fence that was never submitted.
    void Init(bool signaled = false, VulkanContext* context = nullptr);
    void ShutDown();

    /// Blocks until the fence is signalled or \p timeout nanoseconds elapse.
    void Wait(u64 timeout = UINT64_MAX);
    /// Returns the fence to the unsignalled state so it can be resubmitted.
    void Reset();
    [[nodiscard]] bool IsSignaled();

    [[nodiscard]] vk::Fence GetHandle() const { return *m_Fence; }

private:
    VulkanContext* m_context = nullptr;
    vk::raii::Fence m_Fence = nullptr;
};
} // namespace UHE::RHI::VULKAN
