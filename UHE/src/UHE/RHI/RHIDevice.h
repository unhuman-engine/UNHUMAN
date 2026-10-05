#pragma once
#include <memory>
#include "RHITypes.h"
#include "UHE/Core/Core.h"

namespace UHE::RHI
{

class RHICommandBuffer;

/**
 * \brief Backend-agnostic rendering device.
 *
 * The RHI (Render Hardware Interface) is the only rendering surface the engine
 * touches; every Vulkan/DX12/Metal detail stays hidden behind it. Resources cross
 * the boundary as plain descriptors and are referenced by opaque handle types, so
 * a second backend can be added without leaking API types upward.
 *
 * \note Handles returned by the Create* methods are borrowed: they remain valid
 *       until the matching Destroy* call and are not meant to be dereferenced.
 * \see  https://docs.vulkan.org/spec/latest/
 */
class UHE_API RHIDevice
{
public:
    virtual ~RHIDevice() = default;

    /// Creates the device for \p backend and opens the swapchain in \p swapDesc.
    [[nodiscard]] static std::unique_ptr<RHIDevice> Create(Backend backend, const SwapchainDesc& swapDesc);

    // ── Frame lifecycle ──────────────────────────────────────────────────────

    /// Opens a frame: acquires the next swapchain image and resets the per-frame command buffer.
    virtual void Begin() = 0;
    /// Closes and presents the frame opened by Begin().
    virtual void End() = 0;

    /**
     * \returns true when Begin() could not acquire an image (for example an
     *          out-of-date swapchain) and the caller must skip GPU work for this frame.
     * \note    Resize policy — recreate the swapchain and drop the frame instead of stalling.
     */
    [[nodiscard]] virtual bool FrameSkipped() const { return false; }

    /// Blocks the CPU until every piece of GPU work submitted by this device has completed.
    virtual void WaitIdle() = 0;
    /// Rewinds the per-frame command buffers so they can be recorded again.
    virtual void ResetCommandBuffers() = 0;
    /// \returns the index of the frame currently in flight (0 .. frames-in-flight - 1).
    [[nodiscard]] virtual u32 GetCurrentFrameIndex() const = 0;

    // ── Resource creation ────────────────────────────────────────────────────

    virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
    /// \returns the bindless slot assigned to \p handle, or an invalid index when bindless is off.
    virtual u32 GetBufferBindlessIndex(BufferHandle handle) = 0;
    virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual ShaderHandle CreateShader(const ShaderDesc& desc) = 0;
    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;

    // ── Data transfer ────────────────────────────────────────────────────────

    /// Reads a single pixel (RGBA8) back from \p handle into \p outData. Stalls the pipeline.
    virtual void ReadPixel(TextureHandle handle, int x, int y, void* outData) = 0;

    // ── Resource destruction ─────────────────────────────────────────────────

    virtual void DestroyBuffer(BufferHandle handle) = 0;
    virtual void DestroyTexture(TextureHandle handle) = 0;
    virtual void DestroyShader(ShaderHandle handle) = 0;
    virtual void DestroyGraphicsPipeline(PipelineHandle handle) = 0;
    virtual void DestroyComputePipeline(PipelineHandle handle) = 0;

    // ── Command buffer access ────────────────────────────────────────────────

    /// \returns the command buffer bound to the current frame. Valid between Begin() and End().
    virtual RHICommandBuffer& GetCurrentCommandBuffer() = 0;
};

} // namespace UHE::RHI
