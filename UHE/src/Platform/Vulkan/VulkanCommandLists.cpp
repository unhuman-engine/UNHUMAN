#include "uhepch.h"
#include "Platform/Vulkan/VulkanCommandLists.h"
#include "Platform/Vulkan/VulkanDevice.h"

namespace UHE::RHI::VULKAN
{

void VulkanCommandLists::Init(VulkanDevice& device, const VulkanContext* context)
{
    m_Device = &device;
    m_Context = context;
}

void VulkanCommandLists::Shutdown()
{
    std::scoped_lock lock(m_Mutex);
    for (auto& fences : m_Pending)
        fences.clear();
    m_Pools.clear();
}

u32 VulkanCommandLists::FamilyOf(QueueType queue) const
{
    auto& logical = m_Device->getLogicalDevClass();
    switch (queue)
    {
        case QueueType::Compute:
            return logical.getComputeQueueFamilyIndex();
        case QueueType::Transfer:
            return logical.getTransferQueueFamilyIndex();
        case QueueType::Graphics:
            break;
    }
    return logical.getGraphicsQueueFamilyIndex();
}

vk::raii::Queue& VulkanCommandLists::QueueOf(QueueType queue)
{
    auto& logical = m_Device->getLogicalDevClass();
    switch (queue)
    {
        case QueueType::Compute:
            return logical.getComputeQueue();
        case QueueType::Transfer:
            return logical.getTransferQueue();
        case QueueType::Graphics:
            break;
    }
    return logical.getGraphicsQueue();
}

VulkanCommandLists::Pool& VulkanCommandLists::PoolFor(const Key& key)
{
    std::scoped_lock lock(m_Mutex);
    std::unique_ptr<Pool>& pool = m_Pools[key];
    if (!pool)
    {
        pool = std::make_unique<Pool>();
        pool->Handle = vk::raii::CommandPool(
            m_Device->getLogicalDevClass().getLogicalDevice(),
            vk::CommandPoolCreateInfo{.flags = {}, .queueFamilyIndex = FamilyOf(std::get<1>(key))});
    }
    return *pool;
}

void VulkanCommandLists::BeginFrame(u32 slot)
{
    slot %= kSlots;
    std::vector<vk::raii::Fence> fences;
    {
        std::scoped_lock lock(m_Mutex);
        fences.swap(m_Pending[slot]);
    }
    if (!fences.empty())
    {
        std::vector<vk::Fence> handles;
        for (const vk::raii::Fence& fence : fences)
            handles.push_back(*fence);
        (void)m_Device->getLogicalDevClass().getLogicalDevice().waitForFences(handles, VK_TRUE, UINT64_MAX);
    }
    std::scoped_lock lock(m_Mutex);
    for (auto& [key, pool] : m_Pools)
    {
        if (std::get<0>(key) != slot || (pool->PrimaryUsed == 0 && pool->SecondaryUsed == 0))
            continue;
        pool->Handle.reset({});
        pool->PrimaryUsed = 0;
        pool->SecondaryUsed = 0;
    }
}

VulkanCommandBuffer* VulkanCommandLists::Begin(const CommandListDesc& desc, u32 slot)
{
    Pool& pool = PoolFor(Key{slot % kSlots, desc.Queue, desc.ThreadIndex});
    // The pool is this thread's alone from here: no lock while recording.
    const bool primary = desc.Level == CommandListLevel::Primary;
    std::deque<VulkanCommandBuffer>& lists = primary ? pool.Primary : pool.Secondary;
    std::size_t& used = primary ? pool.PrimaryUsed : pool.SecondaryUsed;
    if (used == lists.size())
    {
        VulkanCommandBuffer& list = lists.emplace_back();
        vk::raii::CommandBuffers allocated(
            m_Device->getLogicalDevClass().getLogicalDevice(),
            vk::CommandBufferAllocateInfo{.commandPool = *pool.Handle,
                                          .level = primary ? vk::CommandBufferLevel::ePrimary
                                                           : vk::CommandBufferLevel::eSecondary,
                                          .commandBufferCount = 1});
        list.GetHandle() = std::move(allocated[0]);
        list.SetContext(&m_Device->getLogicalDevClass().getLogicalDevice(), m_Device->GetDescriptorManager(),
                        m_Context);
    }
    VulkanCommandBuffer& list = lists[used++];
    list.BeginList(desc);
    return &list;
}

void VulkanCommandLists::Submit(const SubmitDesc& desc, u32 slot)
{
    std::vector<vk::CommandBuffer> handles;
    for (RHICommandBuffer* list : desc.Lists)
    {
        auto* vulkanList = static_cast<VulkanCommandBuffer*>(list);
        if (vulkanList == nullptr)
            continue;
        UHE_CORE_ASSERT(!vulkanList->IsSecondary(), "Submit: a secondary list runs from ExecuteSecondary");
        vulkanList->End();
        handles.push_back(*vulkanList->GetHandle());
    }
    if (handles.empty())
        return;
    vk::raii::Fence fence(m_Device->getLogicalDevClass().getLogicalDevice(), vk::FenceCreateInfo{});
    {
        std::scoped_lock lock(m_QueueMutex);
        QueueOf(desc.Queue)
            .submit(vk::SubmitInfo{.commandBufferCount = static_cast<u32>(handles.size()),
                                   .pCommandBuffers = handles.data()},
                    *fence);
    }
    if (desc.Wait)
    {
        (void)m_Device->getLogicalDevClass().getLogicalDevice().waitForFences({*fence}, VK_TRUE, UINT64_MAX);
        return;
    }
    std::scoped_lock lock(m_Mutex);
    m_Pending[slot % kSlots].push_back(std::move(fence));
}

} // namespace UHE::RHI::VULKAN
