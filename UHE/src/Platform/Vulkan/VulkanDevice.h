#pragma once
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraph.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphExecutor.h"
#include "Platform/Vulkan/VulkanBarrierEncoder.h"
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDescriptorManager.h"
#include "Platform/Vulkan/VulkanExtensionCheck.h"
#include "Platform/Vulkan/VulkanFrameContext.h"
#include "Platform/Vulkan/VulkanInstance.h"
#include "Platform/Vulkan/VulkanLogicalDevice.h"
#include "Platform/Vulkan/VulkanPhysicalDevice.h"
#include "Platform/Vulkan/VulkanPipelineState.h"
#include "Platform/Vulkan/VulkanSwapChain.h"
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHIDevice.h"

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
    [[nodiscard]] VulkanRenderGraph& GetFrameGraph() { return m_FrameGraph; }
    [[nodiscard]] bool FrameSkipped() const override { return m_FrameSkipped; }

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
    [[nodiscard]] inline const u32& ImageIndex() { return m_ImageIndex; }
    [[nodiscard]] inline VulkanDescriptorManager* GetDescriptorManager() { return &m_DescriptorManager; }
    [[nodiscard]] inline VulkanContext& GetVulkanContext() { return m_Context; }
    void ImmediateSubmit(std::function<void(vk::raii::CommandBuffer& cmd)>&& function);

private:
    void InitVulkan(const SwapchainDesc& swapDesc);
    void CleanupVulkan();
    void RecreateSwapchain();
    void RegisterSwapchainResources();
    void RegisterFrameTextureImpl(RGTextureHandle rgTexture, TextureHandle texture);

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
    bool m_FrameSkipped = false;            // acquire failed → recreate the swapchain and skip the frame
    bool m_FrameGraphFailed = false;        // graph compile/resolve failed → recovery path
    bool m_LegacySwapchainRendered = false; // legacy BeginRenderPass wrote the swapchain this frame

    // Render graph execution (§8.5/§8.6): resolves RG handles to the live
    // swapchain images each frame and encodes compiler-derived barriers.
    VulkanRenderGraphExecutor m_RenderGraphExecutor;
    VulkanBarrierEncoder m_BarrierEncoder;

    // Frame graph + TaskGraph mapping (§14 step 5): per-frame declare →
    // resolve → record; TaskGraph topology refreshed every EndFrameGraph().
    VulkanRenderGraph m_FrameGraph;
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
