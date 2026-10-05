#pragma once
#include <span>
#include <string>
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
};

} // namespace UHE::RHI
