#pragma once

#include <array>
#include <string>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI::VULKAN
{

/**
 * @brief GPU time of each frame graph pass, from timestamp queries.
 *
 * One query pool per frame slot: reset when the slot's command buffer begins,
 * two timestamps per pass (before and after its recording), and read back
 * when the slot comes round again, after its fence has passed, so reading
 * never waits on the GPU. The timings shown are therefore a few frames old.
 * Devices without timestamp support (timestampValidBits 0) report nothing.
 */
class VulkanGpuTimer
{
public:
    static constexpr u32 kMaxPasses = 96;
    static constexpr u32 kSlots = 3;

    void Init(vk::raii::Device& device, vk::raii::PhysicalDevice& physical, u32 queueFamily);
    void Shutdown();

    /// @brief The slot's previous frame has completed: read its timings.
    void Collect(u32 slot);
    /// @brief Start the slot's frame: reset its queries (outside any render pass).
    void BeginFrame(vk::CommandBuffer cmd, u32 slot);
    void BeginPass(vk::CommandBuffer cmd, const std::string& name);
    void EndPass(vk::CommandBuffer cmd);

    [[nodiscard]] const std::vector<GpuPassTiming>& Last() const { return m_Last; }
    [[nodiscard]] bool Supported() const { return m_Supported; }

private:
    struct Slot
    {
        vk::raii::QueryPool Pool = nullptr;
        std::vector<std::string> Names; ///< one per pass, in recording order
        bool Recorded = false;
    };

    bool m_Supported = false;
    double m_NanosecondsPerTick = 1.0;
    u64 m_ValidMask = ~0ull;
    std::array<Slot, kSlots> m_Slots;
    u32 m_Current = 0;
    bool m_InPass = false;
    std::vector<GpuPassTiming> m_Last;
};

} // namespace UHE::RHI::VULKAN
