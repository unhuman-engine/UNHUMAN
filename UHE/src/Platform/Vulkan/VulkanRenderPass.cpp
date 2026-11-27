#include "VulkanRenderPass.h"
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/VulkanContext.h"
#include "UHE/RHI/RHITypes.h"
#include "VulkanTypes.h"

namespace UHE::RHI::VULKAN
{
void VulkanRenderPass::Init(const VulkanContext& ctx, const UHE::RHI::RenderPassDesc& renderdesc)
{
    std::vector<vk::AttachmentDescription> attachments;
    for (u32 i = 0; i < renderdesc.colorAttachmentCount; i++)
    {
        const auto& src = renderdesc.colorAttachments[i];

        attachments.push_back({.flags = {},
                               .format = ToVkFormat(src.format),
                               .samples = ToVkSample(src.sampleCount),
                               .loadOp = ToVkLoadOp(src.loadOp),
                               .storeOp = ToVkStoreOp(src.storeOp),
                               .stencilLoadOp = ToVkLoadOp(src.stencilLoadOp),
                               .stencilStoreOp = ToVkStoreOp(src.stencilStoreOp),
                               .initialLayout = ToVkImageLayout(src.initialusage),
                               .finalLayout = ToVkImageLayout(src.finalusage)});
    }

    const auto vkSubpasses = reinterpret_cast<const vk::SubpassDescription*>(renderdesc.subpasses.data());

    vk::RenderPassCreateInfo RenderPass{
        .flags = {},
        .attachmentCount = renderdesc.colorAttachmentCount + (renderdesc.hasDepth ? 1 : 0),
        .pAttachments = attachments.data(),
        .subpassCount = renderdesc.subpassCount,
        .pSubpasses = vkSubpasses,
        .dependencyCount = renderdesc.dependencyCount,
    };
    m_RenderPass = vk::raii::RenderPass(*ctx.logicalDeviceHandle, RenderPass);
}
}; // namespace UHE::RHI::VULKAN
