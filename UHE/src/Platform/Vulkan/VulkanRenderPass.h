#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHITypes.h"
#include "vulkan/vulkan.hpp"

namespace UHE::RHI::VULKAN
{

struct VulkanContext;

/**
 * \brief Owns a legacy VkRenderPass built from an RHI RenderPassDesc.
 *
 * \todo Prefer dynamic rendering (VK_KHR_dynamic_rendering) and drop this path; it
 *       needs the render-pass builder finished first so old hardware still has a
 *       fallback.
 * \see  https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderPass.html
 */
class VulkanRenderPass
{
public:
    VulkanRenderPass() = default;
    ~VulkanRenderPass() = default;
    VulkanRenderPass(const VulkanRenderPass&) = delete;
    VulkanRenderPass& operator=(const VulkanRenderPass&) = delete;

    void Init(const VulkanContext& ctx, const UHE::RHI::RenderPassDesc& desc);
    [[nodiscard]] vk::RenderPass GetRenderPass() const { return *m_RenderPass; }

private:
    vk::raii::RenderPass m_RenderPass{nullptr};
};

} // namespace UHE::RHI::VULKAN
