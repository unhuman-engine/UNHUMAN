#pragma once
/**
 * @file VulkanCommandLists.h
 * @brief Command lists from per-thread pools, and their submission (issue #30, RHIDevice::BeginCommandList).
 *
 * A command pool may be used by one thread at a time, so each (frame slot,
 * queue, recording thread) has its own pool and its own list of command
 * buffers, reused frame after frame. A slot's pools are reset in BeginFrame,
 * once the frame that last used them finished on the GPU; work submitted from
 * them without a wait carries a fence the slot waits for first.
 */
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/VulkanCommandBuffer.h"
#include "UHE/RHI/RHICore.h"

namespace UHE::RHI::VULKAN
{
class VulkanDevice;

class VulkanCommandLists
{
public:
    static constexpr u32 kSlots = 3; ///< at least MAX_FRAMES_IN_FLIGHT

    void Init(VulkanDevice& device, const VulkanContext* context);
    void Shutdown();

    /// @brief The slot's previous lists finished: wait for their unwaited submits, reset the pools.
    void BeginFrame(u32 slot);

    [[nodiscard]] VulkanCommandBuffer* Begin(const CommandListDesc& desc, u32 slot);
    void Submit(const SubmitDesc& desc, u32 slot);

private:
    struct Pool
    {
        vk::raii::CommandPool Handle{nullptr};
        std::deque<VulkanCommandBuffer> Primary; ///< deque: handed-out pointers stay valid as it grows
        std::deque<VulkanCommandBuffer> Secondary;
        std::size_t PrimaryUsed = 0;
        std::size_t SecondaryUsed = 0;
    };
    using Key = std::tuple<u32, QueueType, u32>; // slot, queue, thread

    Pool& PoolFor(const Key& key);
    [[nodiscard]] u32 FamilyOf(QueueType queue) const;
    [[nodiscard]] vk::raii::Queue& QueueOf(QueueType queue);

    VulkanDevice* m_Device = nullptr;
    const VulkanContext* m_Context = nullptr;
    std::mutex m_Mutex; ///< guards the map and the fence lists, never recording
    std::map<Key, std::unique_ptr<Pool>> m_Pools;
    std::array<std::vector<vk::raii::Fence>, kSlots> m_Pending;
    std::mutex m_QueueMutex; ///< a VkQueue is externally synchronised
};

} // namespace UHE::RHI::VULKAN
