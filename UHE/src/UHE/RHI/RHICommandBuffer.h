#pragma once
#include <span>
#include <string>
#include "RHICore.h"
#include "RHITypes.h"

namespace UHE::RHI
{

/**
 * \brief Backend-agnostic recording context for a single frame.
 *
 * A command buffer records GPU work in submission order: render passes, pipeline
 * bindings, pushes, draws and dispatches. The three data paths (push constants,
 * buffer updates and texture updates) intentionally overlap because different
 * hardware prefers different routes — the backend picks the cheapest one.
 *
 * \note The recording lifetime is Begin() → End(); anything recorded after End()
 *       is undefined. One buffer is reused per frame-in-flight.
 * \see  https://docs.vulkan.org/refpages/latest/refpages/source/VkCommandBuffer.html
 */
class RHICommandBuffer
{
public:
    RHICommandBuffer() = default;
    virtual ~RHICommandBuffer() = default;

    RHICommandBuffer(const RHICommandBuffer&) = delete;
    RHICommandBuffer& operator=(const RHICommandBuffer&) = delete;

    virtual void Begin() = 0;
    virtual void End() = 0;

    // ── Render pass ──────────────────────────────────────────────────────────

    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void EndRenderPass() = 0;

    // ── Pipeline & state bindings ────────────────────────────────────────────

    virtual void BindPipeline(PipelineHandle handle) = 0;
    virtual void BindVertexBuffer(BufferHandle handle, u64 offset = 0) = 0;
    virtual void BindIndexBuffer(BufferHandle handle, u64 offset = 0) = 0;
    virtual void BindTexture(u32 slot, TextureHandle handle) = 0;

    // ── Dynamic state ────────────────────────────────────────────────────────

    virtual void SetViewport(float x, float y, float width, float height) = 0;
    virtual void SetScissor(i32 x, i32 y, u32 width, u32 height) = 0;

    // ── Inline data paths ────────────────────────────────────────────────────

    virtual void PushConstants(ShaderStage stage, const void* data, u32 size, u32 offset = 0) = 0;
    virtual void UpdateBuffer(BufferHandle handle, const void* data, u64 size, u64 offset = 0) = 0;
    virtual void UpdateTexture(TextureHandle handle, std::span<const u8> data) = 0;

    // ── Action commands ──────────────────────────────────────────────────────

    virtual void Draw(u32 vertexCount, u32 firstVertex = 0) = 0;
    virtual void DrawIndexed(u32 indexCount, u32 firstIndex = 0, i32 vertexOffset = 0) = 0;
    virtual void Dispatch(u32 groupCountX, u32 groupCountY = 1, u32 groupCountZ = 1) = 0;

    // ── Thin core (RHICore.h, issue #30) ────────────────────────────────────
    // Nothing below places a barrier or binds anything implicitly. A backend
    // that does not implement a call leaves it a no-op; Vulkan implements all.

    /// @brief Records the given dependencies: the only thing in the core that changes a resource's state.
    virtual void Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers = {},
                         const MemoryDependency* memory = nullptr)
    {
        (void)textures, (void)buffers, (void)memory;
    }

    /// @brief Opens a rendering scope over attachments already in their attachment state; no barrier.
    virtual void BeginRendering(const RenderPassDesc& desc) { (void)desc; }
    virtual void EndRendering() {}
    /// @brief Runs secondary lists recorded for the open rendering scope (or outside one).
    virtual void ExecuteSecondary(std::span<RHICommandBuffer* const> lists) { (void)lists; }

    virtual void CopyBuffer(BufferHandle src, BufferHandle dst, const BufferCopy& region)
    {
        (void)src, (void)dst, (void)region;
    }
    /// @brief Buffer bytes into a texture in TransferDst.
    virtual void CopyBufferToTexture(BufferHandle src, TextureHandle dst, const BufferTextureCopy& region)
    {
        (void)src, (void)dst, (void)region;
    }
    /// @brief Texels of a texture in TransferSrc into a buffer.
    virtual void CopyTextureToBuffer(TextureHandle src, BufferHandle dst, const BufferTextureCopy& region)
    {
        (void)src, (void)dst, (void)region;
    }
    /// @brief Same-size copy between textures in TransferSrc and TransferDst.
    virtual void CopyTexture(TextureHandle src, TextureHandle dst, const TextureCopy& region)
    {
        (void)src, (void)dst, (void)region;
    }
    /// @brief Scaled, filtered copy between textures in TransferSrc and TransferDst.
    virtual void BlitTexture(TextureHandle src, TextureHandle dst, const TextureBlit& region,
                             BlitFilter filter = BlitFilter::Linear)
    {
        (void)src, (void)dst, (void)region, (void)filter;
    }
    /// @brief Resolves a multisampled texture (TransferSrc) into a single-sampled one (TransferDst).
    virtual void ResolveTexture(TextureHandle src, TextureHandle dst) { (void)src, (void)dst; }

    /// @brief Draws with arguments read from a buffer (VkDrawIndirectCommand layout).
    virtual void DrawIndirect(BufferHandle args, u64 offset, u32 drawCount = 1, u32 stride = 16)
    {
        (void)args, (void)offset, (void)drawCount, (void)stride;
    }
    /// @brief DrawIndexed with arguments read from a buffer (VkDrawIndexedIndirectCommand layout).
    virtual void DrawIndexedIndirect(BufferHandle args, u64 offset, u32 drawCount = 1, u32 stride = 20)
    {
        (void)args, (void)offset, (void)drawCount, (void)stride;
    }
    /// @brief Dispatch with the three group counts read from a buffer.
    virtual void DispatchIndirect(BufferHandle args, u64 offset = 0) { (void)args, (void)offset; }

    virtual void ResetQueries(QueryPoolHandle pool, u32 first, u32 count) { (void)pool, (void)first, (void)count; }
    /// @brief Writes the GPU clock once all earlier work reached \p stage.
    virtual void WriteTimestamp(QueryPoolHandle pool, u32 index, Stage stage = Stage::BottomOfPipe)
    {
        (void)pool, (void)index, (void)stage;
    }
    virtual void BeginQuery(QueryPoolHandle pool, u32 index) { (void)pool, (void)index; }
    virtual void EndQuery(QueryPoolHandle pool, u32 index) { (void)pool, (void)index; }

    /// @brief Binds set \p set (1..3) of the bound pipeline: an allocated set from RHIDevice::CreateDescriptorSet.
    virtual void BindDescriptorSet(u32 set, DescriptorSetHandle descriptorSet) { (void)set, (void)descriptorSet; }
    /// @brief Writes set \p set of the bound pipeline in the command stream (a PushDescriptor layout).
    virtual void PushDescriptors(u32 set, std::span<const DescriptorWrite> writes) { (void)set, (void)writes; }

    /// @brief The backend's own command buffer (VkCommandBuffer), for code that must call the API directly.
    [[nodiscard]] virtual void* GetNativeHandle() const { return nullptr; }
};

} // namespace UHE::RHI
