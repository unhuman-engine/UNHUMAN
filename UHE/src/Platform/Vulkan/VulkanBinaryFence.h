#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "UHE/Core/Core.h"

namespace UHE::RHI::VULKAN
{
class VulkanContext;

/**
 * \brief Thin \c vk::raii fence wrapper used by the per-frame submission path.
 *
 * \todo This duplicates VulkanFence (and much of VulkanSemaphore's legacy tier).
 *       Pick one fence type and delete the rest — three overlapping sync wrappers
 *       is exactly the kind of drift that hides real bugs.
 * \see  https://docs.vulkan.org/refpages/latest/refpages/source/VkFence.html
 */
class VulkanBinaryFence
{
public:
    VulkanBinaryFence() = default;
    ~VulkanBinaryFence() = default; // vk::raii::Fence releases itself.

    VulkanBinaryFence(const VulkanBinaryFence&) = delete;
    VulkanBinaryFence& operator=(const VulkanBinaryFence&) = delete;

    void Init(VulkanContext* context, bool signaled = false);
    void Shutdown();

    /// Blocks the CPU until this fence is signalled.
    void WaitOnCpuIn() const;
    /// Returns the fence to the unsignalled state for reuse.
    void ResetIn() const;

    [[nodiscard]] bool IsSignaled() const;
    [[nodiscard]] vk::Fence GetFence() const noexcept { return *m_Fence; }

private:
    VulkanContext* m_context = nullptr;
    vk::raii::Fence m_Fence = nullptr;
};

} // namespace UHE::RHI::VULKAN
