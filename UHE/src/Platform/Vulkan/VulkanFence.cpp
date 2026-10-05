#include "uhepch.h"
#include "VulkanFence.h"
#include "VulkanContext.h"

namespace UHE::RHI::VULKAN
{

void VulkanFence::Init(bool signaled, VulkanContext* context)
{
    m_context = context;
    vk::FenceCreateInfo fenceInfo;
    if (signaled)
    {
        fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
    }

    m_Fence = vk::raii::Fence(*m_context->logicalDeviceHandle, fenceInfo);
}

void VulkanFence::Wait(u64 timeout)
{
    if (*m_Fence)
    {
        // Bounded waits surface device loss instead of hanging the frame loop forever.
        [[maybe_unused]] auto result = m_context->logicalDeviceHandle->waitForFences({*m_Fence}, VK_TRUE, timeout);
    }
}

void VulkanFence::Reset()
{
    if (*m_Fence)
    {
        m_context->logicalDeviceHandle->resetFences({*m_Fence});
    }
}

bool VulkanFence::IsSignaled()
{
    if (*m_Fence)
    {
        return m_Fence.getStatus() == vk::Result::eSuccess;
    }
    return false;
}

void VulkanFence::ShutDown()
{
    m_Fence = nullptr;
    m_context = nullptr;
}

} // namespace UHE::RHI::VULKAN
