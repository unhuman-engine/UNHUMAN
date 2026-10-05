#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanSwapChain.h"
#include "UHE/RHI/RHICommandBuffer.h"
// #include "VulkanCommandPool.h"

namespace UHE::RHI::VULKAN
{
class VulkanDevice;
class VulkanCommandPool;
class VulkanDescriptorManager;

/**
 * \brief Vulkan recording context; the RHICommandBuffer implementation.
 *
 * Records into a vk::raii::CommandBuffer owned by the frame's command pool. The
 * render-pass desc is cached so draw calls can validate state without re-deriving it.
 *
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkCommandBuffer.html
 */
class VulkanCommandBuffer final : public RHICommandBuffer
{
public:
    VulkanCommandBuffer() = default;
    ~VulkanCommandBuffer() override = default;

    VulkanCommandBuffer(const VulkanCommandBuffer&) = delete;
    VulkanCommandBuffer& operator=(const VulkanCommandBuffer&) = delete;

    VulkanCommandBuffer(VulkanCommandBuffer&&) = default;
    VulkanCommandBuffer& operator=(VulkanCommandBuffer&&) = default;

    // ─── Internal Vulkan-specific methods ───────────────────────
    void Allocate(const vk::raii::Device& device, VulkanCommandPool& pool, bool isPrimary = true);
    void Free();
    void BeginCommandBuffer(vk::CommandBufferUsageFlags flags = {});
    void Reset(vk::CommandBufferUsageFlags flags = {});
    void EndCommandBuffer();

    inline const vk::raii::CommandBuffer& GetHandle() const { return m_CommandBuffer; }
    inline vk::raii::CommandBuffer& GetHandle() { return m_CommandBuffer; }

    // ─── RHICommandBuffer overrides ─────────────────────────────
    void Begin() override;
    void End() override;

    // ─── Render Pass ───
    void BeginRenderPass(const RenderPassDesc& desc) override;
    void EndRenderPass() override;

    // ─── Pipeline & State Bindings ───
    void BindPipeline(PipelineHandle handle) override;
    void BindVertexBuffer(BufferHandle handle, u64 offset = 0) override;
    void BindIndexBuffer(BufferHandle handle, u64 offset = 0) override;
    void BindTexture(u32 slot, TextureHandle handle) override;

    // ─── Dynamic States ───
    void SetViewport(float x, float y, float width, float height) override;
    void SetScissor(i32 x, i32 y, u32 width, u32 height) override;

    // ─── Inline Data Paths ───
    void PushConstants(ShaderStage stage, const void* data, u32 size, u32 offset = 0) override;
    void UpdateBuffer(BufferHandle handle, const void* data, u64 size, u64 offset = 0) override;
    void UpdateTexture(TextureHandle handle, std::span<const u8> data) override;

    // ─── Action Commands ───
    void Draw(u32 vertexCount, u32 firstVertex = 0) override;
    void DrawIndexed(u32 indexCount, u32 firstIndex = 0, i32 vertexOffset = 0) override;
    void Dispatch(u32 groupCountX, u32 groupCountY = 1, u32 groupCountZ = 1) override;

    void SetContext(vk::raii::Device* device, VulkanDescriptorManager* descriptorManager, const VulkanContext* ctx)
    {
        m_ctx = ctx;
        m_LogDevice = device;
        m_DescriptorManager = descriptorManager;
    }

private:
    const VulkanContext* m_ctx = nullptr;
    vk::raii::Device* m_LogDevice = nullptr;
    VulkanDescriptorManager* m_DescriptorManager = nullptr;
    vk::PipelineLayout m_CurrentPipelineLayout = nullptr;
    RenderPassDesc m_CurrentRenderPassDesc;
    vk::raii::CommandBuffer m_CommandBuffer{nullptr};
};
} // namespace UHE::RHI::VULKAN
