#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanSwapChain.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHICore.h"
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

    // ─── Thin core (RHICore.h; VulkanCommandBufferCore.cpp) ───
    void Barrier(std::span<const TextureBarrier> textures, std::span<const RHI::BufferBarrier> buffers = {},
                 const MemoryDependency* memory = nullptr) override;
    void BeginRendering(const RenderPassDesc& desc) override;
    void EndRendering() override;
    void ExecuteSecondary(std::span<RHICommandBuffer* const> lists) override;
    void CopyBuffer(BufferHandle src, BufferHandle dst, const BufferCopy& region) override;
    void CopyBufferToTexture(BufferHandle src, TextureHandle dst, const BufferTextureCopy& region) override;
    void CopyTextureToBuffer(TextureHandle src, BufferHandle dst, const BufferTextureCopy& region) override;
    void CopyTexture(TextureHandle src, TextureHandle dst, const TextureCopy& region) override;
    void BlitTexture(TextureHandle src, TextureHandle dst, const TextureBlit& region,
                     BlitFilter filter = BlitFilter::Linear) override;
    void ResolveTexture(TextureHandle src, TextureHandle dst) override;
    void DrawIndirect(BufferHandle args, u64 offset, u32 drawCount = 1, u32 stride = 16) override;
    void DrawIndexedIndirect(BufferHandle args, u64 offset, u32 drawCount = 1, u32 stride = 20) override;
    void DispatchIndirect(BufferHandle args, u64 offset = 0) override;
    void ResetQueries(QueryPoolHandle pool, u32 first, u32 count) override;
    void WriteTimestamp(QueryPoolHandle pool, u32 index, Stage stage = Stage::BottomOfPipe) override;
    void BeginQuery(QueryPoolHandle pool, u32 index) override;
    void EndQuery(QueryPoolHandle pool, u32 index) override;
    void BindDescriptorSet(u32 set, DescriptorSetHandle descriptorSet) override;
    void PushDescriptors(u32 set, std::span<const DescriptorWrite> writes) override;
    [[nodiscard]] void* GetNativeHandle() const override;

    /// @brief Begins a command list from the device's pools (RHIDevice::BeginCommandList).
    void BeginList(const CommandListDesc& desc);
    [[nodiscard]] bool IsSecondary() const { return m_IsSecondary; }
    [[nodiscard]] QueueType GetQueue() const { return m_Queue; }

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
    vk::PipelineBindPoint m_CurrentBindPoint = vk::PipelineBindPoint::eGraphics;
    bool m_IsSecondary = false;
    QueueType m_Queue = QueueType::Graphics;
    RenderPassDesc m_CurrentRenderPassDesc;
    vk::raii::CommandBuffer m_CommandBuffer{nullptr};
};
} // namespace UHE::RHI::VULKAN
