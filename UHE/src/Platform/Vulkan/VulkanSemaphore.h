#pragma once
#include <span>
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/VulkanTypes.h"

namespace UHE::RHI::VULKAN
{
class VulkanContext;

using CommandBufferRef = vk::CommandBuffer;

/// One wait edge of a submit: block \c Semaphore until it reaches \c Value at \c WaitStage.
struct SemaphoreWait
{
    vk::Semaphore Semaphore = nullptr;
    Stage WaitStage = Stage::None;
    u64 Value = 0;
};

/// One signal edge of a submit: raise \c Semaphore to \c Value when the submit completes.
struct SemaphoreSignal
{
    vk::Semaphore Semaphore = nullptr;
    u64 Value = 0;
};

/**
 * \brief Backend-neutral description of a single queue submission.
 *
 * SubmitInfo is what the rest of the engine thinks in — "wait on these, run these,
 * signal these" — regardless of whether the device is on the v1 or Sync2 submit
 * path. VulkanSemaphore::Submit() is the only place that lowers it to
 * vkQueueSubmit2 / vkQueueSubmit.
 *
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit2.html
 */
struct SubmitInfo
{
    std::span<const SemaphoreWait> Waits;
    std::span<const CommandBufferRef> CommandBuffers;
    std::span<const SemaphoreSignal> Signals;
};

/**
 * \brief A binary or timeline semaphore with a device-capability fallback.
 *
 * Timeline semaphores let a submit carry a monotonically increasing value, so the
 * CPU can wait for "frame N done" without a semaphore-per-frame dance. They require
 * Vulkan 1.2 (or VK_KHR_timeline_semaphore) and are only meaningful on the Sync2
 * submit path, so on older devices this class transparently degrades to a plain
 * binary semaphore (the caller's GetWait/GetSignal values are then ignored).
 *
 * \note Only the render thread may Init/ShutDown; WaitCPU is safe to call from any
 *       thread once the semaphore exists.
 * \see  https://docs.vulkan.org/refpages/latest/refpages/source/VkSemaphore.html
 * \see  https://www.khronos.org/blog/vulkan-timeline-semaphores
 */
class VulkanSemaphore
{
public:
    VulkanSemaphore() = default;
    ~VulkanSemaphore() = default;
    VulkanSemaphore(const VulkanSemaphore&) = delete;
    VulkanSemaphore& operator=(const VulkanSemaphore&) = delete;

    /**
     * \brief Creates the semaphore, preferring a timeline when \p requestTimeline is set.
     * \param requestTimeline hint only — a binary semaphore is created when the device
     *        lacks timeline plus Sync2 support (see IsTimeline()).
     * \param initialValue    starting timeline counter value (ignored for binary).
     */
    void Init(bool requestTimeline = false, u64 initialValue = 0, VulkanContext* context = nullptr);
    void ShutDown();

    /**
     * \brief Blocks the CPU until the timeline reaches at least \p value.
     * \returns true on success, false on timeout.
     * \note Always bounded by \p timeoutNs — an infinite wait would hang forever on
     *       device loss, which is exactly when the caller needs to notice.
     * \note On the legacy (binary) tier this is a no-op that returns true; the frame
     *       fence is the real gate there.
     */
    [[nodiscard]] bool WaitCPU(u64 value, u64 timeoutNs);
    /// \returns the current timeline counter value, or 0 on the binary tier.
    u64 GetValue();

    /// Submits \p info to \p queue, lowering it to the path the device supports.
    static void Submit(VulkanContext* context, vk::raii::Queue& queue, const SubmitInfo& info,
                       vk::Fence fence = nullptr);

    [[nodiscard]] vk::Semaphore GetHandle() const { return *m_Semaphore; }
    [[nodiscard]] bool IsTimeline() const { return m_IsTimeline; }

    /// Builds a wait edge on this semaphore at \p stage (and \p value, timeline only).
    [[nodiscard]] SemaphoreWait GetWait(Stage stage, u64 value = 0) const
    {
        return SemaphoreWait{.Semaphore = *m_Semaphore, .WaitStage = stage, .Value = value};
    }

    /// Builds a signal edge on this semaphore (value ignored on the binary tier).
    [[nodiscard]] SemaphoreSignal GetSignal(u64 value = 0) const
    {
        return SemaphoreSignal{.Semaphore = *m_Semaphore, .Value = value};
    }

private:
    bool m_IsTimeline = false; // true only when requested AND the device supports it
    VulkanContext* ctx = nullptr;
    vk::raii::Semaphore m_Semaphore = nullptr;

    // \todo Emulate timeline semaphores on the legacy tier with a ring of binary
    //       semaphores, so the Sync2Timeline-only paths have a fallback.
};

} // namespace UHE::RHI::VULKAN
