#pragma once

namespace UHE::RHI::VULKAN
{
class VulkanContext;

/**
 * \brief Reserved placeholder for a fallback binary-semaphore path.
 *
 * \todo Not implemented and not referenced anywhere. A binary semaphore cannot
 *       carry a value, so emulating a timeline with them needs a ring of them plus
 *       bookkeeping — see the Khronos write-up below. Either implement it as the
 *       legacy tier's fallback or delete it, so the sync layer stops advertising
 *       two ways to do the same thing.
 * \see  https://www.khronos.org/blog/vulkan-timeline-semaphores
 */
class VulkanBinarySemaphore
{
public:
    VulkanBinarySemaphore() = default;
    ~VulkanBinarySemaphore() = default;

    VulkanBinarySemaphore(const VulkanBinarySemaphore&) = delete;
    VulkanBinarySemaphore& operator=(const VulkanBinarySemaphore&) = delete;

    void Init(VulkanContext* ctx);
    void Create();
    void Shutdown();
};

} // namespace UHE::RHI::VULKAN
