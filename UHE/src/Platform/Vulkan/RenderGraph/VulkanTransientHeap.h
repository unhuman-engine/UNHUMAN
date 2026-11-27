#pragma once
/**
 * @file VulkanTransientHeap.h
 * @brief A frame's transient textures in shared memory, packed by lifetime (issue #32).
 *
 * PlanTransientPlacement (UHE/RHI/FrameGraph/TransientPlacement.h) groups the
 * textures whose pass intervals never overlap; each group is one VMA
 * allocation, and its textures are images bound at offset 0 of it
 * (vmaCreateAliasingImage2), whatever their shapes. The images and blocks are
 * kept while frames ask for the same set of textures and lifetimes, which is
 * every frame until a resize or a settings change; then the old ones go to the
 * deferred destruction and a new packing is made.
 *
 * Safe for the same reason same-shape reuse is: the render graph's first
 * barrier on a resource in a frame starts from Undefined and waits on all
 * earlier work, so whatever another texture left in the memory is discarded.
 */
#include <span>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI::VULKAN
{
class VulkanDevice;
class VulkanTexture;

class VulkanTransientHeap
{
public:
    struct Request
    {
        TextureDesc Desc;
        u32 First = 0; ///< first pass that uses the texture
        u32 Last = 0;  ///< last pass that uses it
    };

    struct Stats
    {
        u64 RequestedBytes = 0; ///< one allocation per texture would take this
        u64 AllocatedBytes = 0; ///< the blocks take this
        u32 Textures = 0;
        u32 Blocks = 0;
        u64 Rebuilds = 0; ///< packings made (a resize or a new frame shape)
    };

    explicit VulkanTransientHeap(VulkanDevice& device) : m_Device(device) {}
    ~VulkanTransientHeap() { Clear(); }
    VulkanTransientHeap(const VulkanTransientHeap&) = delete;
    VulkanTransientHeap& operator=(const VulkanTransientHeap&) = delete;

    /// @brief One texture per request, in order; null entries when an allocation failed.
    [[nodiscard]] const std::vector<TextureHandle>& Acquire(std::span<const Request> requests);
    /// @brief Releases every block and texture (deferred: frames in flight may still use them).
    void Clear();

    [[nodiscard]] const Stats& GetStats() const { return m_Stats; }

private:
    [[nodiscard]] bool SameShape(std::span<const Request> requests) const;
    void Build(std::span<const Request> requests);

    VulkanDevice& m_Device;
    std::vector<Request> m_Shape; ///< what the current packing was made for
    std::vector<VmaAllocation> m_Blocks;
    std::vector<VkImage> m_Images;
    std::vector<VulkanTexture*> m_Objects;
    std::vector<TextureHandle> m_Handles;
    Stats m_Stats;
};

} // namespace UHE::RHI::VULKAN
