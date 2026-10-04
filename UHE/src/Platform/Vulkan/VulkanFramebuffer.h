#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "UHE/Core/Core.h"

namespace UHE::RHI::VULKAN
{

struct FramebufferDesc
{
    vk::RenderPass renderPass;
    u32 attachmentCount = 0;
    const vk::ImageView* attachments = nullptr;
    u32 width = 0;
    u32 height = 0;
    u32 layers = 1;
};

/// Owns a VkFramebuffer binding a set of image views to a render pass.
/// \see https://docs.vulkan.org/refpages/latest/refpages/source/VkFramebuffer.html
class VulkanFramebuffer
{
public:
    VulkanFramebuffer() = default;
    ~VulkanFramebuffer() = default;
    VulkanFramebuffer(const VulkanFramebuffer&) = delete;
    VulkanFramebuffer& operator=(const VulkanFramebuffer&) = delete;

    void Init(const FramebufferDesc& desc);
    void Cleanup();

    [[nodiscard]] vk::Framebuffer GetHandle() const { return *m_Framebuffer; }

private:
    vk::raii::Framebuffer m_Framebuffer = nullptr;
};

} // namespace UHE::RHI::VULKAN
