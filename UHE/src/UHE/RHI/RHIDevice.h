#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "RHITypes.h"
#include "UHE/Core/Core.h"
#include "UHE/RHI/FrameGraph/FrameGraphTypes.h"

namespace UHE::RHI
{

class RHICommandBuffer;
class FrameGraph;

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

    /// Tells the device the window's framebuffer changed size. A presentation
    /// engine is not required to report the old swapchain as out of date (many
    /// Wayland compositors never do), so the device must not rely on that alone.
    virtual void OnFramebufferResized() {}

    /// Blocks the CPU until every piece of GPU work submitted by this device has completed.
    virtual void WaitIdle() = 0;
    /// Rewinds the per-frame command buffers so they can be recorded again.
    virtual void ResetCommandBuffers() = 0;
    /// \returns the index of the frame currently in flight (0 .. frames-in-flight - 1).
    [[nodiscard]] virtual u32 GetCurrentFrameIndex() const = 0;

    /// \returns the format the swapchain images were created with.
    ///
    /// A pass that renders straight into the swapchain has one attachment in
    /// this format, and a pipeline has to be created for the pass it is bound
    /// in, so a scene pipeline that also serves such a pass needs it.
    [[nodiscard]] virtual TextureFormat GetSwapchainFormat() = 0;

    // ── Resource creation ────────────────────────────────────────────────────

    virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
    /// \returns the bindless slot assigned to \p handle, or an invalid index when bindless is off.
    virtual u32 GetBufferBindlessIndex(BufferHandle handle) = 0;
    /// Slot of \p handle in the per-material data array (shader binding 2).
    /// The shader's fixed bindless layout keeps material arrays separate from
    /// the light/bone arrays of binding 0 because SPIR-V allows one descriptor
    /// type per binding.
    virtual u32 GetMaterialBufferBindlessIndex(BufferHandle handle) = 0;
    virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual ShaderHandle CreateShader(const ShaderDesc& desc) = 0;
    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;

    // ── Data transfer ────────────────────────────────────────────────────────

    /// Reads a single pixel (RGBA8) back from \p handle into \p outData. Stalls the pipeline.
    virtual void ReadPixel(TextureHandle handle, int x, int y, void* outData) = 0;

    /**
     * @brief Non-blocking pixel read, for picking under the mouse every frame.
     *
     * Queues a copy of texel (\p x, \p y) of \p handle into this frame's
     * command buffer, and writes the newest pixel whose frame has finished on
     * the GPU (a few frames old) into \p outData. Nothing waits.
     *
     * @return false while no request has completed yet (outData untouched).
     * @note The default falls back to the blocking ReadPixel.
     */
    virtual bool ReadPixelAsync(TextureHandle handle, int x, int y, void* outData)
    {
        ReadPixel(handle, x, y, outData);
        return true;
    }

    /// @brief A presented frame read back to the CPU: tightly packed RGBA8, top row first.
    struct CapturedFrame
    {
        u32 Width = 0;
        u32 Height = 0;
        std::vector<u8> Rgba; ///< Width * Height * 4 bytes, sRGB-encoded as presented
    };

    /**
     * @brief Copies the image the current frame presents into CPU memory.
     *
     * Call between Begin() and End(). End() records the copy after everything
     * else in the frame (ImGui included), waits for that one submission, and
     * calls \p onCaptured before it returns. A skipped frame keeps the request
     * for the next one. This stalls the GPU: it is for tests such as the golden
     * image check, not for per-frame use.
     *
     * @return false when the backend cannot read its swapchain back; \p onCaptured
     *         is then never called.
     */
    virtual bool RequestFrameCapture(std::function<void(const CapturedFrame&)> onCaptured)
    {
        (void)onCaptured;
        return false;
    }

    // ── Resource destruction ─────────────────────────────────────────────────

    virtual void DestroyBuffer(BufferHandle handle) = 0;
    virtual void DestroyTexture(TextureHandle handle) = 0;
    virtual void DestroyShader(ShaderHandle handle) = 0;
    virtual void DestroyGraphicsPipeline(PipelineHandle handle) = 0;
    virtual void DestroyComputePipeline(PipelineHandle handle) = 0;

    // ── Command buffer access ────────────────────────────────────────────────

    /// \returns the command buffer bound to the current frame. Valid between Begin() and End().
    virtual RHICommandBuffer& GetCurrentCommandBuffer() = 0;

    // ── Frame graph ──────────────────────────────────────────────────────────

    /**
     * @brief The frame graph passes are declared into this frame, or nullptr without one.
     *
     * Declare between Begin() and End(), from a layer's OnUpdate. The device
     * realizes the frame before the UI pass, so these passes record first, and
     * resets the graph at the end of every frame: handles do not carry over.
     */
    [[nodiscard]] virtual FrameGraph* GetFrameGraph() { return nullptr; }
    /// @brief Size and format of the image this frame presents, for FrameGraph::ImportBackbuffer.
    [[nodiscard]] virtual FGTexture::Desc GetBackbufferDesc() { return {}; }
    /**
     * @brief The texture the frame graph keeps as \p name (FrameGraph::Export), or nullptr.
     *
     * Call after the export is declared: during that frame it is already the
     * object the graph will render into, so the UI of the same frame samples
     * this frame's image. The object changes when the export's size or format
     * does; ask every frame instead of keeping it.
     */
    [[nodiscard]] virtual TextureHandle GetExportedTexture(const std::string& name)
    {
        (void)name;
        return nullptr;
    }
};

} // namespace UHE::RHI
