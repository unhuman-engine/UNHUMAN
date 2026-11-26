#include "uhepch.h"
#include "Platform/Vulkan/VulkanGpuTimer.h"

namespace UHE::RHI::VULKAN
{

void VulkanGpuTimer::Init(vk::raii::Device& device, vk::raii::PhysicalDevice& physical, u32 queueFamily)
{
    const vk::PhysicalDeviceProperties properties = physical.getProperties();
    const std::vector<vk::QueueFamilyProperties> families = physical.getQueueFamilyProperties();
    const u32 validBits = queueFamily < families.size() ? families[queueFamily].timestampValidBits : 0;
    m_Supported = validBits > 0 && properties.limits.timestampPeriod > 0.0f;
    if (!m_Supported)
    {
        UHE_CORE_WARN("GPU timer: this queue has no timestamps; pass timings are unavailable");
        return;
    }
    m_NanosecondsPerTick = static_cast<double>(properties.limits.timestampPeriod);
    m_ValidMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1ull);
    for (Slot& slot : m_Slots)
    {
        const vk::QueryPoolCreateInfo info{
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = kMaxPasses * 2,
        };
        slot.Pool = vk::raii::QueryPool(device, info);
    }
}

void VulkanGpuTimer::Shutdown()
{
    for (Slot& slot : m_Slots)
    {
        slot.Pool = nullptr;
        slot.Names.clear();
        slot.Recorded = false;
    }
    m_Last.clear();
}

void VulkanGpuTimer::Collect(u32 slotIndex)
{
    if (!m_Supported)
        return;
    Slot& slot = m_Slots[slotIndex % kSlots];
    if (!slot.Recorded || slot.Names.empty())
        return;
    slot.Recorded = false;
    const u32 count = static_cast<u32>(slot.Names.size()) * 2;
    // The slot's fence passed before this call: the results are available,
    // and asking without the wait flag cannot stall the CPU.
    const auto [result, ticks] =
        slot.Pool.getResults<u64>(0, count, count * sizeof(u64), sizeof(u64), vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess)
        return;
    std::vector<GpuPassTiming> timings;
    timings.reserve(slot.Names.size());
    for (size_t i = 0; i < slot.Names.size(); ++i)
    {
        const u64 begin = ticks[i * 2] & m_ValidMask;
        const u64 end = ticks[i * 2 + 1] & m_ValidMask;
        const double nanoseconds = end >= begin ? static_cast<double>(end - begin) * m_NanosecondsPerTick : 0.0;
        timings.push_back({slot.Names[i], static_cast<float>(nanoseconds / 1.0e6)});
    }
    m_Last = std::move(timings);
}

void VulkanGpuTimer::BeginFrame(vk::CommandBuffer cmd, u32 slotIndex)
{
    m_Current = slotIndex % kSlots;
    m_InPass = false;
    if (!m_Supported)
        return;
    Slot& slot = m_Slots[m_Current];
    slot.Names.clear();
    slot.Recorded = false;
    cmd.resetQueryPool(*slot.Pool, 0, kMaxPasses * 2);
}

void VulkanGpuTimer::BeginPass(vk::CommandBuffer cmd, const std::string& name)
{
    if (!m_Supported || m_InPass)
        return;
    Slot& slot = m_Slots[m_Current];
    if (slot.Names.size() >= kMaxPasses)
        return; // past the pool: unmeasured, not an error
    const u32 query = static_cast<u32>(slot.Names.size()) * 2;
    slot.Names.push_back(name);
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *slot.Pool, query);
    m_InPass = true;
}

void VulkanGpuTimer::EndPass(vk::CommandBuffer cmd)
{
    if (!m_Supported || !m_InPass)
        return;
    Slot& slot = m_Slots[m_Current];
    const u32 query = static_cast<u32>(slot.Names.size() - 1) * 2 + 1;
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *slot.Pool, query);
    slot.Recorded = true;
    m_InPass = false;
}

} // namespace UHE::RHI::VULKAN
