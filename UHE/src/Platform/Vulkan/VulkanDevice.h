#pragma once
#include <mutex>
#include <optional>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/RenderGraph/VulkanFrameGraphBridge.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphExecutor.h"
#include "Platform/Vulkan/VulkanBarrierEncoder.h"
#include "Platform/Vulkan/VulkanCommandLists.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDescriptorManager.h"
#include "Platform/Vulkan/VulkanExtensionCheck.h"
#include "Platform/Vulkan/VulkanFrameContext.h"
#include "Platform/Vulkan/VulkanGpuTimer.h"
#include "Platform/Vulkan/VulkanInstance.h"
#include "Platform/Vulkan/VulkanLogicalDevice.h"
#include "Platform/Vulkan/VulkanPhysicalDevice.h"
#include "Platform/Vulkan/VulkanPipelineState.h"
#include "Platform/Vulkan/VulkanSwapChain.h"
#include "UHE/Core/Core.h"
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/RHI/RenderGraph/RenderGraph.h"

namespace UHE::RHI::VULKAN
{

class VulkanBuffer;

/**
 * \brief Vulkan implementation of RHIDevice; owns the whole backend object graph.
 *
 * Created by RHIDevice::Create and torn down with the renderer. It builds the
 * instance / physical / logical device, swapchain, descriptor manager, per-frame
 * contexts, render graph and the sync-tier selection, then drives the Begin/End loop.
 *
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateInstance.html
 */
class UHE_API VulkanDevice final : public RHIDevice
{
public:
    VulkanDevice(const SwapchainDesc& swapDesc);
    ~VulkanDevice() override;

    // ─── Frame Lifecycle ────────────────────────────────────────
    void Begin() override;
    void End() override;
    void WaitIdle() override;
    void ResetCommandBuffers() override;
    uint32_t GetCurrentFrameIndex() const override { return m_CurrentFrame; }

    // ─── Resource Creation ──────────────────────────────────────
    BufferHandle CreateBuffer(const BufferDesc& desc) override;
    u32 GetBufferBindlessIndex(BufferHandle handle) override;
    u32 GetMaterialBufferBindlessIndex(BufferHandle handle) override;
    TextureHandle CreateTexture(const TextureDesc& desc) override;
    ShaderHandle CreateShader(const ShaderDesc& desc) override;
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;

    void ReadPixel(TextureHandle handle, int x, int y, void* outData) override;
    bool ReadPixelAsync(TextureHandle handle, int x, int y, void* outData) override;
    bool RequestFrameCapture(std::function<void(const CapturedFrame&)> onCaptured) override;

    void DestroyBuffer(BufferHandle handle) override;
    void DestroyTexture(TextureHandle handle) override;
    void DestroyShader(ShaderHandle handle) override;
    void DestroyGraphicsPipeline(PipelineHandle handle) override;
    void DestroyComputePipeline(PipelineHandle handle) override;
    void DeferDestruction(std::function<void()>&& function);
    u32 RegisterBuffer(VulkanBuffer* buffer);
    /// Registers \p buffer into the material-buffer array (binding 2).
    u32 RegisterMaterialBuffer(VulkanBuffer* buffer);

    // ─── Command Buffer Access ──────────────────────────────────
    RHICommandBuffer& GetCurrentCommandBuffer() override;

    // ─── Render graph (§14 steps 4–5) ──────────────────────────
    // Executor resolves/records graph passes into the frame's primary command
    // buffer. Swapchain images are registered under RGTextureHandle{imageIndex,1}
    // every Begin(); transient graph resources are registered by their owner
    // right after creation. See rendergraph.md §8.5/§8.6.
    [[nodiscard]] VulkanRenderGraphExecutor& GetRenderGraphExecutor() { return m_RenderGraphExecutor; }
    // Per-frame graph feature passes declare into (M5 step 4); Reset() by
    // EndFrameGraph() after recording.
    [[nodiscard]] RenderGraph& GetRenderGraph() { return m_FrameGraph; }
    /// \copydoc RHIDevice::GetFrameGraph
    [[nodiscard]] FrameGraph* GetFrameGraph() override { return &m_Frontend; }
    /// @brief Binds \p buffer to a render graph buffer slot for this frame.
    void RegisterFrameBuffer(RGBufferHandle rgBuffer, BufferHandle buffer);
    /// \copydoc RHIDevice::GetBackbufferDesc
    [[nodiscard]] FGTexture::Desc GetBackbufferDesc() override;
    /// \copydoc RHIDevice::GetExportedTexture
    [[nodiscard]] TextureHandle GetExportedTexture(const std::string& name) override;
    /// @brief Imports this frame's swapchain image into the render graph. Every
    /// importer (the UI pass, a frame graph backbuffer) gets the same slot.
    RGTextureHandle ImportSwapchain();
    [[nodiscard]] bool FrameSkipped() const override { return m_FrameSkipped; }
    /// Recreates the swapchain after the next present (\copydoc RHIDevice::OnFramebufferResized).
    void OnFramebufferResized() override { m_FramebufferResized = true; }
    /// \copydoc RHIDevice::SetVSync
    void SetVSync(bool enabled) override;
    [[nodiscard]] bool IsVSync() const override { return m_VSync; }

    // Set by VulkanCommandBuffer::BeginRenderPass when a frame's legacy scene
    // pass rendered directly into the swapchain (leaving it in the Present
    // layout). BeginImGuiPass reads it to choose the swapchain import's initial
    // state; reset every Begin().
    void MarkSwapchainRenderedByLegacy() { m_LegacySwapchainRendered = true; }
    [[nodiscard]] bool SwapchainRenderedByLegacy() const { return m_LegacySwapchainRendered; }

    // Declares/refreshes the ImGui pass (§14 step 5): executor opens the
    // swapchain scope from this declaration; the compiler owns the layout
    // transitions. Called from VulkanImGuiLayer::End after host-side Render.
    void BeginImGuiPass(const std::string& name);

    // RG handle of the swapchain import for the frame in progress. Stored by
    // BeginImGuiPass so RegisterSwapchainResources() binds the live image to
    // the exact slot the graph declared (imports are slot-sequential, the raw
    // image index is not).

    // ─── Feature-pass migration (M5 step 4) ────────────────────
    // Binds an engine texture to an RG handle for the frame (image + view +
    // format from the live object). Owners call this once per frame per
    // resource right after creating their RG declarations.
    void RegisterFrameTexture(RGTextureHandle rgTexture, TextureHandle texture);

    // Declares → compiles → resolves → TaskGraph-maps → records the frame's
    // graph passes onto the frame's primary command buffer (§14 step 5).
    void EndFrameGraph();

    // ─── Window Management ────────────────────────────────────────
    [[nodiscard]] GLFWwindow* GetWindowHandle() const { return m_WindowHandle; }

    void GetLogicalDeviceInfo(u32& vendorID, u32& deviceID) const
    {
        return m_PhysicalDevice.GetLogicalDeviceInfo(vendorID, deviceID);
    };

    [[nodiscard]] inline vk::raii::Queue& GetGraphicsQueue() { return m_LogicalDevice.getGraphicsQueue(); }
    [[nodiscard]] inline VulkanLogicalDevice& getLogicalDevClass() { return m_LogicalDevice; }
    [[nodiscard]] inline VulkanInstance& getInstanceClass() { return m_Instance; }
    [[nodiscard]] inline VulkanPhysicalDevice& getPhysicalDevClass() { return m_PhysicalDevice; }
    [[nodiscard]] inline VulkanSwapChain& getSwapChainClass() { return m_SwapChain; }
    /// \copydoc RHIDevice::GetSwapchainFormat
    [[nodiscard]] TextureFormat GetSwapchainFormat() override { return m_SwapChain.GetSwapchainFormat(); }
    /// \copydoc RHIDevice::GetMaxPushConstantsSize
    [[nodiscard]] u32 GetMaxPushConstantsSize() const override;
    /// \copydoc RHIDevice::GetGpuPassTimings
    [[nodiscard]] const std::vector<GpuPassTiming>& GetGpuPassTimings() const override { return m_GpuTimer.Last(); }
    /// @brief Timestamps around one frame graph pass's recording (the bridge calls these).
    void BeginGpuPass(const std::string& name);
    void EndGpuPass();
    [[nodiscard]] bool SupportsWireframe() const override { return m_ExtensionCheck.SupportsWireframe(); }

    // ── Thin core (RHICore.h; VulkanDeviceCore.cpp) ─────────────────────────
    [[nodiscard]] const DeviceLimits& GetLimits() const override;
    [[nodiscard]] void* GetNativeHandle(NativeObject object) const override;
    [[nodiscard]] void* GetNativeTextureHandle(TextureHandle texture, NativeTextureObject object) const override;
    [[nodiscard]] void* GetNativeBufferHandle(BufferHandle buffer) const override;
    bool ReadBuffer(BufferHandle buffer, u64 offset, std::span<u8> out) override;
    TextureHandle ImportTexture(const TextureImportDesc& desc) override;
    TextureHandle CreateTextureView(TextureHandle texture, const TextureViewDesc& desc) override;
    QueryPoolHandle CreateQueryPool(const QueryPoolDesc& desc) override;
    void DestroyQueryPool(QueryPoolHandle pool) override;
    bool GetQueryResults(QueryPoolHandle pool, u32 first, std::span<u64> out, bool wait = false) override;
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override;
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle layout) override;
    DescriptorSetHandle CreateDescriptorSet(DescriptorSetLayoutHandle layout) override;
    void UpdateDescriptorSet(DescriptorSetHandle set, std::span<const DescriptorWrite> writes) override;
    void DestroyDescriptorSet(DescriptorSetHandle set) override;
    RHICommandBuffer* BeginCommandList(const CommandListDesc& desc) override;
    void Submit(const SubmitDesc& desc) override;
    [[nodiscard]] inline const u32& ImageIndex() { return m_ImageIndex; }
    [[nodiscard]] inline VulkanDescriptorManager* GetDescriptorManager() { return &m_DescriptorManager; }
    [[nodiscard]] inline VulkanBarrierEncoder& GetBarrierEncoder() { return m_BarrierEncoder; }
    [[nodiscard]] inline VulkanContext& GetVulkanContext() { return m_Context; }
    void ImmediateSubmit(std::function<void(vk::raii::CommandBuffer& cmd)>&& function);

private:
    void InitVulkan(const SwapchainDesc& swapDesc);
    void CleanupVulkan();
    void RecreateSwapchain();
    void RegisterSwapchainResources();
    void RegisterFrameTextureImpl(RGTextureHandle rgTexture, TextureHandle texture);
    /// @brief Declares the frontend's passes into the render graph, once per frame.
    void FlushFrontend();
    /// @brief Resets the frontend for the next frame and ages the transient pool.
    void EndFrontendFrame();
    /// @brief Records this frame's queued pixel copy, if any, after the graph.
    void RecordPixelReadback(vk::raii::CommandBuffer& cmd);
    /// @brief Takes the result of this slot's previous copy; its fence has passed.
    void CollectPixelReadback();
    /// @brief Records the copy of the presented image a capture asked for.
    /// @return true when a copy was recorded and must be collected after submit.
    bool RecordFrameCapture(vk::raii::CommandBuffer& cmd);
    /// @brief Waits for this frame's submission and hands the pixels to the callback.
    void CollectFrameCapture();

    // ─── Immediate Submission (Uploads) ───────────────────────────

private:
    VulkanContext m_Context;

    // Core Vulkan abstractions
    VulkanInstance m_Instance;
    VulkanPhysicalDevice m_PhysicalDevice;
    VulkanLogicalDevice m_LogicalDevice;
    VulkanSwapChain m_SwapChain;
    VmaAllocator m_Allocator = nullptr;
    VulkanDescriptorManager m_DescriptorManager;
    VulkanExtensionCheck m_ExtensionCheck;
    VulkanPipelineStateCache m_PipelineStateCache;

    GLFWwindow* m_WindowHandle = nullptr;
    u32 m_WindowWidth;
    u32 m_WindowHeight;

    // Frame-in-flight sync
    static constexpr u32 MAX_FRAMES_IN_FLIGHT = 2;
    std::array<VulkanFrameContext, MAX_FRAMES_IN_FLIGHT> m_Frames;
    std::vector<vk::raii::Semaphore> m_RenderFinishedSemaphores; // binary, one per swapchain image
    // Frame timeline per frame slot: CPU pacing before touching slot-indexed
    // resources. Inert on the Legacy tier (binary + fence pacing).
    std::array<VulkanSemaphore, MAX_FRAMES_IN_FLIGHT> m_FrameTimelines;
    std::array<u64, MAX_FRAMES_IN_FLIGHT> m_FrameTimelineValues{}; // next value to signal
    u32 m_CurrentFrame = 0;
    u32 m_ImageIndex = 0; // Current swapchain image index
    bool m_FramebufferResized = false;
    bool m_VSync = true;                    // FIFO when set; MAILBOX or IMMEDIATE otherwise
    bool m_FrameSkipped = false;            // acquire failed → recreate the swapchain and skip the frame
    bool m_FrameGraphFailed = false;        // graph compile/resolve failed → recovery path
    bool m_LegacySwapchainRendered = false; // legacy BeginRenderPass wrote the swapchain this frame

    // Render graph execution (§8.5/§8.6): resolves RG handles to the live
    // swapchain images each frame and encodes compiler-derived barriers.
    VulkanRenderGraphExecutor m_RenderGraphExecutor;
    VulkanGpuTimer m_GpuTimer;
    VulkanBarrierEncoder m_BarrierEncoder;
    VulkanCommandLists m_CommandLists;
    bool m_PerPassLists = true;                 ///< UHE_RENDERGRAPH_PRIMARY=1: every pass into the frame's primary
    VulkanSemaphore m_ComputeTimeline;          ///< async compute work done (issue #32)
    u64 m_ComputeTimelineValue = 0;             ///< last value signalled
    std::optional<SemaphoreWait> m_ComputeWait; ///< what this frame's graphics submission waits for
    void SubmitAsyncCompute(const RGCompiledFrame& compiled, const RGResolvedFrame& resolved);
    mutable DeviceLimits m_Limits;
    mutable std::once_flag m_LimitsOnce;

    // Frame graph + TaskGraph mapping (§14 step 5): per-frame declare →
    // resolve → record; TaskGraph topology refreshed every EndFrameGraph().
    RenderGraph m_FrameGraph;
    // FrameGraph frontend: declared by layers, realized on m_FrameGraph once
    // per frame by the bridge (FlushFrontend), reset at the end of End().
    FrameGraph m_Frontend;
    VulkanFrameGraphBridge m_FrameGraphBridge{*this};
    bool m_FrontendFlushed = false;
    Jobsystem::UheJobsystem m_Jobsystem;
    Jobsystem::TaskGraph m_TaskGraph;
    std::vector<Jobsystem::TaskID> m_PassNodes;
    // Imported swapchain identity — bumped per re-import so a resize (new
    // vk::SwapchainKHR) changes the topology hash and rebuilds the cache.
    u64 m_SwapchainGeneration = 0;
    // The RG slot the swapchain import actually occupies this frame (BeginImGuiPass
    // stores it). Registration must key on THIS handle — the raw image index is
    // not the RG slot index (imports allocate sequentially from 0).
    RGTextureHandle m_SwapchainRGHandle{};

    /// @brief One 4-byte readback per frame slot, for ReadPixelAsync.
    struct PixelReadback
    {
        vk::Buffer Buffer = nullptr;
        VmaAllocation Allocation = VK_NULL_HANDLE;
        bool InFlight = false; ///< a copy was submitted and not yet collected
    };
    std::array<PixelReadback, MAX_FRAMES_IN_FLIGHT> m_PixelReadbacks{};
    /// @brief The copy asked for during the current frame, recorded in End().
    struct PixelRequest
    {
        TextureHandle Texture = nullptr;
        int X = 0;
        int Y = 0;
    };
    std::optional<PixelRequest> m_PixelRequest;
    std::optional<u32> m_LastPixel; ///< newest collected value

    /// @brief A pending RequestFrameCapture and the staging buffer of its copy.
    struct FrameCapture
    {
        std::function<void(const CapturedFrame&)> Callback;
        vk::Buffer Buffer = nullptr;
        VmaAllocation Allocation = VK_NULL_HANDLE;
        u32 Width = 0;
        u32 Height = 0;
        bool SwapRedBlue = false; ///< the swapchain is BGRA
    };
    FrameCapture m_FrameCapture;

    // Immediate submit context
    vk::raii::Fence m_UploadFence = nullptr;
    vk::raii::CommandPool m_UploadCommandPool = nullptr;
    vk::raii::CommandBuffer m_UploadCommandBuffer = nullptr;

    class VulkanGraphicPipeline* m_CurrentPipeline = nullptr;

    enum class VendorID
    {
        VENDOR_ID_AMD = 0x1002,
        VENDOR_ID_NVIDIA = 0x10de,
        VENDOR_ID_INTEL = 0x8086,
        VENDOR_ID_ARM = 0x13b5,
        VENDOR_ID_QCOM = 0x5143
    };
};

} // namespace UHE::RHI::VULKAN
