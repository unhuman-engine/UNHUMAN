#include "uhepch.h"
#include "VulkanBinaryFence.h"
#include "VulkanContext.h"

namespace UHE::RHI::VULKAN
{

void VulkanBinaryFence::Init(VulkanContext* context, bool signaled)
{
    m_context = context;

    vk::FenceCreateInfo info{};
    if (signaled)
    {
        info.flags = vk::FenceCreateFlagBits::eSignaled;
    }

    m_Fence = vk::raii::Fence(*m_context->logicalDeviceHandle, info);
}

void VulkanBinaryFence::Shutdown()
{
    m_Fence = nullptr;
    m_context = nullptr;
}

void VulkanBinaryFence::WaitOnCpuIn() const
{
    if (m_context && *m_Fence)
    {
        (void)m_context->logicalDeviceHandle->waitForFences({*m_Fence}, VK_TRUE, UINT64_MAX);
    }
}

void VulkanBinaryFence::ResetIn() const
{
    if (m_context && *m_Fence)
    {
        m_context->logicalDeviceHandle->resetFences({*m_Fence});
    }
}

bool VulkanBinaryFence::IsSignaled() const
{
    if (!m_context || !*m_Fence)
    {
        return false;
    }
    return m_Fence.getStatus() == vk::Result::eSuccess;
}

} // namespace UHE::RHI::VULKAN
