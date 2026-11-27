#include "uhepch.h"
#include "Platform/Vulkan/VulkanBarrierEncoder.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanExtensionCheck.h"

namespace UHE::RHI::VULKAN
{

void VulkanBarrierEncoder::Init(VulkanContext* context)
{
    m_Context = context;
}

void VulkanBarrierEncoder::Shutdown()
{
    m_Context = nullptr;
}

void VulkanBarrierEncoder::Encode(vk::CommandBuffer commandBuffer, std::span<const ImageBarrier> imageBarriers,
                                  std::span<const BufferBarrier> bufferBarriers, const GlobalBarrier* globalBarrier)
{
    if (imageBarriers.empty() && bufferBarriers.empty() && globalBarrier == nullptr)
        return;

    const SyncTier tier = m_Context != nullptr ? m_Context->CheckExtensions->GetSyncTier() : SyncTier::Legacy;

    if (tier == SyncTier::Legacy)
    {
        // v1 barriers carry access masks but not stage masks, so the stages are
        // OR'd across the group and passed to the single pipelineBarrier call.
        vk::PipelineStageFlags srcStages{};
        vk::PipelineStageFlags dstStages{};

        std::vector<vk::ImageMemoryBarrier> images;
        images.reserve(imageBarriers.size());
        for (const ImageBarrier& barrier : imageBarriers)
        {
            srcStages |= ToVkPipelineStage1(barrier.SrcStage);
            dstStages |= ToVkPipelineStage1(barrier.DstStage);

            images.push_back({
                .srcAccessMask = ToVkAccess1(barrier.SrcAccess),
                .dstAccessMask = ToVkAccess1(barrier.DstAccess),
                .oldLayout = ToVkImageLayout(barrier.Old),
                .newLayout = ToVkImageLayout(barrier.Next),
                .srcQueueFamilyIndex = barrier.SrcQueueFamily,
                .dstQueueFamilyIndex = barrier.DstQueueFamily,
                .image = barrier.Image,
                .subresourceRange = barrier.Range,
            });
        }

        std::vector<vk::BufferMemoryBarrier> buffers;
        buffers.reserve(bufferBarriers.size());
        for (const BufferBarrier& barrier : bufferBarriers)
        {
            srcStages |= ToVkPipelineStage1(barrier.SrcStage);
            dstStages |= ToVkPipelineStage1(barrier.DstStage);

            buffers.push_back({
                .srcAccessMask = ToVkAccess1(barrier.SrcAccess),
                .dstAccessMask = ToVkAccess1(barrier.DstAccess),
                .srcQueueFamilyIndex = barrier.SrcQueueFamily,
                .dstQueueFamilyIndex = barrier.DstQueueFamily,
                .buffer = barrier.Buffer,
                .offset = barrier.Offset,
                .size = barrier.Size,
            });
        }

        std::vector<vk::MemoryBarrier> globals;
        if (globalBarrier != nullptr)
        {
            srcStages |= ToVkPipelineStage1(globalBarrier->SrcStage);
            dstStages |= ToVkPipelineStage1(globalBarrier->DstStage);

            globals.push_back({
                .srcAccessMask = ToVkAccess1(globalBarrier->SrcAccess),
                .dstAccessMask = ToVkAccess1(globalBarrier->DstAccess),
            });
        }

        commandBuffer.pipelineBarrier(srcStages, dstStages, vk::DependencyFlags{}, globals, buffers, images);
        return;
    }

    std::vector<vk::ImageMemoryBarrier2> images;
    images.reserve(imageBarriers.size());
    for (const ImageBarrier& barrier : imageBarriers)
    {
        images.push_back({
            .srcStageMask = ToVkPipelineStage2(barrier.SrcStage),
            .srcAccessMask = ToVkAccess2(barrier.SrcAccess),
            .dstStageMask = ToVkPipelineStage2(barrier.DstStage),
            .dstAccessMask = ToVkAccess2(barrier.DstAccess),
            .oldLayout = ToVkImageLayout(barrier.Old),
            .newLayout = ToVkImageLayout(barrier.Next),
            .srcQueueFamilyIndex = barrier.SrcQueueFamily,
            .dstQueueFamilyIndex = barrier.DstQueueFamily,
            .image = barrier.Image,
            .subresourceRange = barrier.Range,
        });
    }

    std::vector<vk::BufferMemoryBarrier2> buffers;
    buffers.reserve(bufferBarriers.size());
    for (const BufferBarrier& barrier : bufferBarriers)
    {
        buffers.push_back({
            .srcStageMask = ToVkPipelineStage2(barrier.SrcStage),
            .srcAccessMask = ToVkAccess2(barrier.SrcAccess),
            .dstStageMask = ToVkPipelineStage2(barrier.DstStage),
            .dstAccessMask = ToVkAccess2(barrier.DstAccess),
            .srcQueueFamilyIndex = barrier.SrcQueueFamily,
            .dstQueueFamilyIndex = barrier.DstQueueFamily,
            .buffer = barrier.Buffer,
            .offset = barrier.Offset,
            .size = barrier.Size,
        });
    }

    std::vector<vk::MemoryBarrier2> globals;
    if (globalBarrier != nullptr)
    {
        globals.push_back({
            .srcStageMask = ToVkPipelineStage2(globalBarrier->SrcStage),
            .srcAccessMask = ToVkAccess2(globalBarrier->SrcAccess),
            .dstStageMask = ToVkPipelineStage2(globalBarrier->DstStage),
            .dstAccessMask = ToVkAccess2(globalBarrier->DstAccess),
        });
    }

    const vk::DependencyInfo dependency{
        .memoryBarrierCount = static_cast<u32>(globals.size()),
        .pMemoryBarriers = globals.data(),
        .bufferMemoryBarrierCount = static_cast<u32>(buffers.size()),
        .pBufferMemoryBarriers = buffers.data(),
        .imageMemoryBarrierCount = static_cast<u32>(images.size()),
        .pImageMemoryBarriers = images.data(),
    };

    commandBuffer.pipelineBarrier2(dependency);
}

} // namespace UHE::RHI::VULKAN
