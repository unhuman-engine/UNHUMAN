#include "uhepch.h"
#include "VulkanDevice.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <iostream>

// ─── Frame-loop timeouts (§9.1.5: bounded waits, never UINT64_MAX) ──────────
// One second of GPU silence on a live submission means the device is gone or a
// deadlock happened — fail loudly instead of hanging the app.
static constexpr u64 kFramePacingTimeoutNs = 1'000'000'000ull;
static constexpr u64 kAcquireTimeoutNs = 1'000'000'000ull;

// Reverse of MapTextureFormat for the formats the swapchain can expose
// (executor registration carries the engine enum for clear-value typing).
static UHE::RHI::TextureFormat ToEngineFormat(vk::Format format)
{
    using UHE::RHI::TextureFormat;
    switch (format)
    {
        case vk::Format::eB8G8R8A8Unorm:
            return TextureFormat::BGRA8_UNORM;
        case vk::Format::eB8G8R8A8Srgb:
            return TextureFormat::BGRA8_SRGB;
        case vk::Format::eR8G8B8A8Unorm:
            return TextureFormat::RGBA8_UNORM;
        case vk::Format::eR8G8B8A8Srgb:
            return TextureFormat::RGBA8_SRGB;
        default:
            return TextureFormat::RGBA8_UNORM;
    }
}

// #include <atomic>
#include <common/TracyQueue.hpp>
#include <cstdint>
#ifdef _WIN32
// Windows-specific includes if needed
#else
    #include <unistd.h>
#endif
#include <volk.h>
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/UI/VulkanImGuiPass.h"
#include "Platform/Vulkan/ValidationGate.h"
#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanComputePipeline.h"
#include "Platform/Vulkan/VulkanExtensionCheck.h"
#include "Platform/Vulkan/VulkanGraphicPipeline.h"
#include "Platform/Vulkan/VulkanPipelineState.h"
#include "Platform/Vulkan/VulkanShader.h"
#include "Platform/Vulkan/VulkanTexture.h"
#include "Platform/Vulkan/VulkanUtils.h"
#include "UHE/Core/Log.h"
#include "UHE/RHI/RHITypes.h"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE;

namespace UHE::RHI::VULKAN
{

VulkanDevice::VulkanDevice(const SwapchainDesc& swapDesc)
{
    m_WindowHandle = static_cast<GLFWwindow*>(swapDesc.nativeWindow);
    m_WindowWidth = swapDesc.width;
    m_WindowHeight = swapDesc.height;
    m_VSync = swapDesc.vsync;
    InitVulkan(swapDesc);
}

VulkanDevice::~VulkanDevice()
{
    CleanupVulkan();
}

void VulkanDevice::InitVulkan(const SwapchainDesc& swapDesc)
{
    UHE_PROFILE_FUNCTION();

    m_Instance.initialize();

    // Initialize Vulkan-Hpp default dispatcher
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*m_Instance.getInstance());

    m_LogicalDevice.CreateSurface(m_Instance, m_WindowHandle);
    m_PhysicalDevice.initPhysicalDevice(m_Instance);

    m_LogicalDevice.initialize(m_PhysicalDevice, *m_LogicalDevice.getSurface(), m_Instance, m_ExtensionCheck);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(*m_LogicalDevice.getLogicalDevice());

    // The frame graph drives the frame loop (scene + ImGui passes) and
    // VulkanRenderGraphExecutor::RecordRange always uses vkCmdBeginRendering —
    // there is no render-pass/framebuffer fallback. Reject devices that cannot
    // support it up front instead of silently dropping every graph pass.
    if (!m_ExtensionCheck.SupportsDynamicRendering())
    {
        throw std::runtime_error(
            "VulkanDevice requires VK_KHR_dynamic_rendering (Vulkan 1.3): the render graph has no fallback");
    }

    m_Allocator = m_LogicalDevice.getAllocator();

    m_SwapChain.createSwapChain(m_LogicalDevice.getLogicalDevice(), m_PhysicalDevice.getPhysicalDevice(),
                                m_LogicalDevice.getSurface(), m_WindowHandle, m_VSync);

    for (auto& frame : m_Frames)
    {
        frame.Init(m_LogicalDevice.getLogicalDevice(), m_LogicalDevice.getGraphicsQueueFamilyIndex());
    }

    m_RenderFinishedSemaphores.clear();
    for (size_t i = 0; i < m_SwapChain.GetImages().size(); i++)
    {
        vk::SemaphoreCreateInfo semaphoreInfo{.flags = {}};
        m_RenderFinishedSemaphores.emplace_back(m_LogicalDevice.getLogicalDevice(), semaphoreInfo);
    }

    vk::CommandPoolCreateInfo uploadPoolInfo{.flags = {},
                                             .queueFamilyIndex = m_LogicalDevice.getGraphicsQueueFamilyIndex()};
    m_UploadCommandPool = vk::raii::CommandPool(m_LogicalDevice.getLogicalDevice(), uploadPoolInfo);

    vk::FenceCreateInfo fenceInfo{.flags = {}};
    m_UploadFence = vk::raii::Fence(m_LogicalDevice.getLogicalDevice(), fenceInfo);

    m_Context.instance = &m_Instance;
    m_Context.physicalDevice = &m_PhysicalDevice;
    m_Context.logicalDevice = &m_LogicalDevice;
    m_Context.CheckExtensions = &m_ExtensionCheck;
    m_Context.swapChain = &m_SwapChain;
    m_Context.device = this;
    m_Context.graphicPipeline = m_CurrentPipeline;
    m_Context.descriptorManager = &m_DescriptorManager;
    m_Context.fallbackDescriptorPool = m_DescriptorManager.GetFallbackPool();
    m_Context.allocator = m_Allocator;
    m_Context.logicalDeviceHandle = &m_LogicalDevice.getLogicalDevice();
    m_Context.physicalDeviceHandle = &m_PhysicalDevice.getPhysicalDevice();
    m_Context.instanceHandle = &m_Instance.getInstance();
    m_Context.graphicsQueue = &m_LogicalDevice.getGraphicsQueue();
    m_Context.surface = &m_LogicalDevice.getSurface();
    m_Context.graphicsQueueFamilyIndex = m_LogicalDevice.getGraphicsQueueFamilyIndex();

    // Frame timelines (§9.1.1/D1 v1): one per frame slot, CPU-observable pacing.
    // On the Legacy tier these degrade to binary semaphores and the timeline
    // wait becomes a no-op — the in-flight fence stays authoritative there.
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        m_FrameTimelines[i].Init(/*requestTimeline=*/true, 0, &m_Context);
        // Next signal is 1: timeline values must STRICTLY increase (signaling 0
        // on a 0-valued semaphore is a spec violation — VUID-…-03882 — and
        // device-losts the submit).
        m_FrameTimelineValues[i] = 1;
    }
    m_BarrierEncoder.Init(&m_Context);
    m_Jobsystem.Init();

    UHE_CORE_INFO("Vulkan sync tier: {}", SyncTierName(m_ExtensionCheck.GetSyncTier()));

    g_VulkanContext = &m_Context;

    m_DescriptorManager.init(*this);

    for (auto& frame : m_Frames)
    {
        frame.GetCommandBuffer().SetContext(&m_LogicalDevice.getLogicalDevice(), &m_DescriptorManager, &m_Context);
    }

    UHE_CORE_INFO("Vulkan device initialized successfully");
}

void VulkanDevice::CleanupVulkan()
{
    g_VulkanContext = nullptr;

    WaitIdle();

    // Pooled frame graph textures go the way of any texture: through the
    // deletion queues the frame cleanup below flushes.
    m_Frontend.Reset();
    m_FrameGraphBridge.Shutdown();

    m_DescriptorManager.cleanup();

    for (PixelReadback& readback : m_PixelReadbacks)
    {
        if (readback.Buffer)
            vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(readback.Buffer), readback.Allocation);
        readback = {};
    }

    for (auto& frame : m_Frames)
    {
        frame.Cleanup();
    }

    m_UploadFence = nullptr;
    m_UploadCommandPool = nullptr;
    m_RenderFinishedSemaphores.clear();
    m_BarrierEncoder.Shutdown();
    for (auto& timeline : m_FrameTimelines)
        timeline.ShutDown();
    m_Jobsystem.ShutDown();

    m_SwapChain.cleanupSwapChain();
    m_LogicalDevice.cleanup();
}

void VulkanDevice::RecreateSwapchain()
{
    UHE_PROFILE_FUNCTION();

    int width = 0, height = 0;
    glfwGetFramebufferSize(m_WindowHandle, &width, &height);
    while (width == 0 || height == 0)
    {
        glfwGetFramebufferSize(m_WindowHandle, &width, &height);
        glfwWaitEvents();
    }

    WaitIdle();
    m_SwapChain.cleanupSwapChain();
    m_SwapChain.createSwapChain(m_LogicalDevice.getLogicalDevice(), m_PhysicalDevice.getPhysicalDevice(),
                                m_LogicalDevice.getSurface(), m_WindowHandle, m_VSync);

    // §9.1.4: registrations bound to the dead swapchain are gone — next
    // Begin() re-registers the fresh images (which is why resize rebuilds the
    // graph's cache: imported handles' identity changed).
    m_RenderGraphExecutor.ClearRegistrations();
    // The imported swapchain's identity changes with the vk::SwapchainKHR —
    // bump here (NOT per frame: a stable identity is what lets the §8.3 cache
    // hit across unchanged frames).
    ++m_SwapchainGeneration;

    m_RenderFinishedSemaphores.clear();
    for (size_t i = 0; i < m_SwapChain.GetImages().size(); i++)
    {
        vk::SemaphoreCreateInfo semaphoreInfo{.flags = {}};
        m_RenderFinishedSemaphores.emplace_back(m_LogicalDevice.getLogicalDevice(), semaphoreInfo);
    }
}

void VulkanDevice::SetVSync(bool enabled)
{
    if (enabled == m_VSync)
        return;
    m_VSync = enabled;
    // The present mode is fixed at swapchain creation: take the resize path,
    // which recreates the swapchain right after the next present.
    m_FramebufferResized = true;
}

// ─── Resource Management Stubs  ───

BufferHandle VulkanDevice::CreateBuffer(const BufferDesc& desc)
{
    auto* buffer = new VulkanBuffer();

    vk::BufferUsageFlags usage{};
    if (desc.usage == BufferUsage::Vertex)
    {
        usage = vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    }
    else if (desc.usage == BufferUsage::Index)
    {
        usage = vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
    }
    else if (desc.usage == BufferUsage::Uniform)
    {
        usage = vk::BufferUsageFlagBits::eUniformBuffer;
    }
    else if (desc.usage == BufferUsage::Storage)
    {
        // Transfer both ways too: a storage buffer is also what a frame graph
        // pass copies into or out of.
        usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc |
                vk::BufferUsageFlagBits::eTransferDst;
    }
    else if (desc.usage == BufferUsage::Staging)
    {
        usage = vk::BufferUsageFlagBits::eTransferSrc;
    }

    VmaMemoryUsage memUsage = desc.hostVisible ? VMA_MEMORY_USAGE_CPU_TO_GPU : VMA_MEMORY_USAGE_GPU_ONLY;
    buffer->init(m_Allocator, desc.size, usage, memUsage);
    return reinterpret_cast<BufferHandle>(buffer);
}

u32 VulkanDevice::RegisterBuffer(VulkanBuffer* buffer)
{
    if (!m_ExtensionCheck.Supports(Extension::DescriptorIndexing))
    {
        buffer->SetBindlessIndex(static_cast<u32>(-1));
        return static_cast<u32>(-1);
    }

    u32 index =
        m_DescriptorManager.RegisterBuffer(m_LogicalDevice.getLogicalDevice(), buffer->GetHandle(), buffer->GetSize());
    buffer->SetBindlessIndex(index);
    return index;
}

u32 VulkanDevice::RegisterMaterialBuffer(VulkanBuffer* buffer)
{
    if (!m_ExtensionCheck.Supports(Extension::DescriptorIndexing))
    {
        buffer->SetMaterialBindlessIndex(static_cast<u32>(-1));
        return static_cast<u32>(-1);
    }

    u32 index = m_DescriptorManager.RegisterMaterialBuffer(m_LogicalDevice.getLogicalDevice(), buffer->GetHandle(),
                                                           buffer->GetSize());
    buffer->SetMaterialBindlessIndex(index);
    return index;
}

u32 VulkanDevice::GetMaterialBufferBindlessIndex(BufferHandle handle)
{
    if (!handle)
        return static_cast<u32>(-1);

    auto* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    u32 index = buffer->GetMaterialBindlessIndex();

    if (index == static_cast<u32>(-1))
    {
        index = RegisterMaterialBuffer(buffer);
    }

    return index;
}

u32 VulkanDevice::GetBufferBindlessIndex(BufferHandle handle)
{
    if (!handle)
        return static_cast<u32>(-1);

    // the handle points to a VulkanBuffer instance. We return its stored bindless index.
    auto* buffer = reinterpret_cast<VulkanBuffer*>(handle);
    u32 index = buffer->GetBindlessIndex();

    if (index == static_cast<u32>(-1))
    {
        index = RegisterBuffer(buffer);
    }

    return index;
}

TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc)
{
    auto* texture = new VulkanTexture();
    texture->Init(*this, desc);
    return reinterpret_cast<TextureHandle>(texture);
}
ShaderHandle VulkanDevice::CreateShader(const ShaderDesc& desc)
{
    auto* shader = new VulkanShader();
    shader->Create(m_LogicalDevice.getLogicalDevice(), desc);
    return reinterpret_cast<ShaderHandle>(shader);
}
PipelineHandle VulkanDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    return m_PipelineStateCache.Acquire(
        desc,
        [this, &desc]()
        {
            auto* pipeline = new VulkanGraphicPipeline();
            pipeline->createGraphicsPipeline(m_LogicalDevice, m_DescriptorManager, m_Context, desc);
            return reinterpret_cast<PipelineHandle>(static_cast<VulkanPipelineState*>(pipeline));
        });
}

PipelineHandle VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& desc)
{
    return m_PipelineStateCache.Acquire(desc,
                                        [this, &desc]()
                                        {
                                            auto* pipeline = new VulkanComputePipeline();
                                            pipeline->CreateComputePipeline(m_LogicalDevice, m_DescriptorManager, desc);
                                            return reinterpret_cast<PipelineHandle>(
                                                static_cast<VulkanPipelineState*>(pipeline));
                                        });
}

void VulkanDevice::DestroyBuffer(BufferHandle handle)
{
    if (handle)
    {
        auto* buffer = reinterpret_cast<VulkanBuffer*>(handle);
        m_Frames[m_CurrentFrame].GetDeletionQueue().Push([buffer]() { delete buffer; });
    }
}

void VulkanDevice::DestroyTexture(TextureHandle handle)
{
    if (handle)
    {
        auto* texture = reinterpret_cast<VulkanTexture*>(handle);
        m_Frames[m_CurrentFrame].GetDeletionQueue().Push([texture]() { delete texture; });
    }
}

void VulkanDevice::DestroyShader(ShaderHandle handle)
{
    if (handle)
    {
        auto* shader = reinterpret_cast<VulkanShader*>(handle);
        m_Frames[m_CurrentFrame].GetDeletionQueue().Push([shader]() { delete shader; });
    }
}

void VulkanDevice::DestroyGraphicsPipeline(PipelineHandle handle)
{
    if (!handle || !m_PipelineStateCache.Release(handle))
        return;

    auto* pipeline = reinterpret_cast<VulkanPipelineState*>(handle);
    m_Frames[m_CurrentFrame].GetDeletionQueue().Push([pipeline]() { delete pipeline; });
}

void VulkanDevice::DestroyComputePipeline(PipelineHandle handle)
{
    if (!handle || !m_PipelineStateCache.Release(handle))
        return;

    auto* pipeline = reinterpret_cast<VulkanPipelineState*>(handle);
    m_Frames[m_CurrentFrame].GetDeletionQueue().Push([pipeline]() { delete pipeline; });
}

void VulkanDevice::DeferDestruction(std::function<void()>&& function)
{
    m_Frames[m_CurrentFrame].GetDeletionQueue().Push(std::move(function));
}

void VulkanDevice::Begin()
{
    UHE_PROFILE_FUNCTION();
    m_FrameSkipped = false;
    m_FrameGraphFailed = false;
    m_LegacySwapchainRendered = false;
    m_RenderGraphExecutor.ClearRegistrations();

    // ── Pacing (§9.1.1): wait for this frame slot's previous use. The timeline
    // wait is the CPU-observable path on the Sync2 tier and a no-op on Legacy;
    // the in-flight fence remains the authoritative gate on both (§9.1.1 v1:
    // delays, never drops). Deletion flush happens only AFTER pacing confirmed
    // (§9.1.7) — the slot's resources are free to reuse from here on.
    if (m_FrameTimelineValues[m_CurrentFrame] > 0)
    {
        const bool paced =
            m_FrameTimelines[m_CurrentFrame].WaitCPU(m_FrameTimelineValues[m_CurrentFrame] - 1, kFramePacingTimeoutNs);
        if (!paced)
            UHE_CORE_ERROR("VulkanDevice::Begin: frame timeline wait timed out (device lost?)");
    }

    auto waitResult = m_LogicalDevice.getLogicalDevice().waitForFences({*m_Frames[m_CurrentFrame].GetInFlightFence()},
                                                                       VK_TRUE, UINT64_MAX);
    UHE_CORE_ASSERT(waitResult == vk::Result::eSuccess, "Failed to wait for in-flight fence!");
    CollectPixelReadback(); // this slot's fence passed: its copy has landed

    // ── Acquire ONCE (§9.1.4): no retry loop. Out-of-date → recreate + skip
    // the frame; the fresh swapchain is acquired on the next Begin.
    vk::Result acquireResult = vk::Result::eSuccess;
    uint32_t imageIndex = 0;

    try
    {
        auto [res, idx] = m_SwapChain.GetSwapchain().acquireNextImage(
            kAcquireTimeoutNs, *m_Frames[m_CurrentFrame].GetimageAvailableSemaphore(), nullptr);
        acquireResult = res;
        imageIndex = idx;
    }
    catch (const vk::OutOfDateKHRError&)
    {
        acquireResult = vk::Result::eErrorOutOfDateKHR;
    }

    if (acquireResult == vk::Result::eErrorOutOfDateKHR)
    {
        RecreateSwapchain();
        m_FrameSkipped = true; // §9.1.4: recreate + skip, never spin
    }
    else if (acquireResult == vk::Result::eTimeout || acquireResult == vk::Result::eNotReady)
    {
        // Not an error: the presentation engine had no image for us within the
        // timeout (a compositor may withhold them while the window is hidden).
        // Nothing was acquired and no semaphore will be signalled, so skip the
        // frame exactly like the out-of-date case, without recreating.
        UHE_CORE_WARN("VulkanDevice::Begin: no swapchain image within {0} ms, skipping the frame",
                      kAcquireTimeoutNs / 1'000'000);
        m_FrameSkipped = true;
    }
    else if (acquireResult != vk::Result::eSuccess && acquireResult != vk::Result::eSuboptimalKHR)
    {
        throw std::runtime_error("Failed to acquire swap chain image!");
    }

    m_ImageIndex = imageIndex;
    m_Context.currentFrameIndex = m_CurrentFrame;
    m_Context.imageIndex = m_ImageIndex;

    m_Frames[m_CurrentFrame].GetDeletionQueue().Flush();
    m_Frames[m_CurrentFrame].GetCommandBuffer().Reset();
    m_Frames[m_CurrentFrame].GetCommandBuffer().BeginCommandBuffer(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
}

void VulkanDevice::End()
{
    UHE_PROFILE_FUNCTION();
    // §14 step 5: graph passes record into the same primary buffer before it
    // is ended (single-queue v1 — the TaskGraph overlaps CPU recording with
    // the previous work, but submit must wait for all nodes). Skipped frames
    // (§9.1.4) must NOT run the graph: nothing was acquired, the registration
    // table belongs to a different image index, and recording would either
    // resolve against stale objects or index the swapchain out of bounds.
    if (!m_FrameSkipped)
        EndFrameGraph();
    // Every frame, skipped or failed included: the frontend's pass data must
    // not leak into the next frame, and its handles must go stale.
    EndFrontendFrame();

    vk::raii::CommandBuffer& cmd = m_Frames[m_CurrentFrame].GetCommandBuffer().GetHandle();

    if (m_FrameGraphFailed && !m_LegacySwapchainRendered)
    {
        // The graph is what would have transitioned the acquired image to
        // Present, but it failed before recording anything. Emit that
        // transition here so the frame can still be submitted and presented —
        // consuming the acquire semaphore and releasing the image — instead of
        // being dropped with an acquired-but-unpresented image.
        TransitionLayout(cmd, m_SwapChain.GetImages()[m_ImageIndex], vk::ImageLayout::eUndefined,
                         vk::ImageLayout::ePresentSrcKHR, vk::AccessFlags{}, vk::AccessFlags{},
                         vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eBottomOfPipe);
    }
    m_FrameGraphFailed = false;

    if (!m_FrameSkipped)
        RecordPixelReadback(cmd);
    m_PixelRequest.reset();
    // A skipped frame presents nothing, so a capture waits for the next one.
    const bool capturing = !m_FrameSkipped && RecordFrameCapture(cmd);

    cmd.end();

    if (m_FrameSkipped)
    {
        // §9.1.4: nothing was acquired — no submit, no present. The (empty or
        // partial) command buffer is discarded; the frame slot advances. The
        // in-flight fence is left signaled (it is reset only immediately before
        // a real submit below), so the next Begin() on this slot cannot hang.
        m_FrameGraph.Reset();
        m_FrameSkipped = false;
        m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
        return;
    }

    // Fence reset is deferred to here rather than done in Begin(): a skipped
    // frame never submits, so leaving the (already-signaled) fence alone keeps
    // the next Begin()'s wait a no-op.
    m_LogicalDevice.getLogicalDevice().resetFences({*m_Frames[m_CurrentFrame].GetInFlightFence()});

    // ── Acquire/present edges (§9.1.3): wait imageAvailable[frame] at
    // ColorAttachmentOutput; signal renderFinished[image] (binary, per swapchain
    // image) plus the frame timeline. On the Sync2 tier the timeline signal
    // carries value+1; on Legacy it degrades to a plain binary signal and the
    // value is ignored. Tier choice happens inside VulkanSemaphore::Submit.
    // imageAvailable is a binary semaphore owned by the frame context; the
    // wait shape is built inline (timeline value ignored, §9.1.3).
    const SemaphoreWait acquireWait{.Semaphore = *m_Frames[m_CurrentFrame].GetimageAvailableSemaphore(),
                                    .WaitStage = Stage::ColorOutput,
                                    .Value = 0};
    const SemaphoreSignal renderDone =
        SemaphoreSignal{.Semaphore = *m_RenderFinishedSemaphores[m_ImageIndex], .Value = 0};
    const SemaphoreSignal framePaced =
        m_FrameTimelines[m_CurrentFrame].GetSignal(m_FrameTimelineValues[m_CurrentFrame]);
    const SemaphoreWait waits[] = {acquireWait};
    const SemaphoreSignal signals[] = {renderDone, framePaced};

    vk::raii::Queue& graphicsQueue = m_LogicalDevice.getGraphicsQueue();
    VulkanRenderGraphExecutor::Submit(&m_Context, graphicsQueue, *cmd, waits, signals,
                                      *m_Frames[m_CurrentFrame].GetInFlightFence());

    // The timeline value is consumed: the next signal on this slot is value+1.
    ++m_FrameTimelineValues[m_CurrentFrame];

    vk::PresentInfoKHR presentInfo{.waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &(*m_RenderFinishedSemaphores[m_ImageIndex]),
                                   .swapchainCount = 1,
                                   .pSwapchains = &(*m_SwapChain.GetSwapchain()),
                                   .pImageIndices = &m_ImageIndex,
                                   .pResults = nullptr};

    try
    {
        auto presentResult = graphicsQueue.presentKHR(presentInfo);
        if (presentResult == vk::Result::eErrorOutOfDateKHR || presentResult == vk::Result::eSuboptimalKHR ||
            m_FramebufferResized)
        {
            m_FramebufferResized = false;
            RecreateSwapchain();
        }
    }
    catch (vk::OutOfDateKHRError&)
    {
        m_FramebufferResized = false;
        RecreateSwapchain();
    }
    if (capturing)
        CollectFrameCapture();
    m_CurrentFrame = (m_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

RHICommandBuffer& VulkanDevice::GetCurrentCommandBuffer()
{
    return m_Frames[m_CurrentFrame].GetCommandBuffer();
}

void VulkanDevice::BeginImGuiPass(const std::string& name)
{
    // Frontend passes first: they render what the UI draws on top of.
    FlushFrontend();
    // §14 step 5: declare the ImGui pass into the per-frame graph. The executor
    // opens the swapchain scope from this declaration (Load preserves the scene
    // that drew ahead of us; Clear would erase it), and the compiler owns the
    // ColorAttachment→Present exit transition (§13.5 item 28 defect is gone).
    // Migration note: the swapchain import starts at ColorAttachment because
    // legacy scene rendering still draws ahead of this pass; it flips to
    // Present-based acquire when scene passes go graph-resident (ROADMAP M5
    // step 4).
    RGTextureHandle swap = ImportSwapchain();

    auto& pass = m_FrameGraph.AddPass(name, RGPassType::Graphics);
    // The attachment target must also be declared written (§7): Write() first,
    // then Color() re-binds to the version this pass just produced. Without
    // the Write(), validation reports UndeclaredAttachment and the whole frame
    // fails to compile (nothing recorded → the swapchain never returns to
    // Present → the next acquire blocks forever).
    pass.Write(swap)
        .Color({swap, LoadOp::Load, StoreOp::Store, {0, 0, 0, 1}})
        .Execute([](RGPassContext& context) { VulkanImGuiPass::RecordInContext(context); });
}

void VulkanDevice::RegisterFrameTexture(RGTextureHandle rgTexture, TextureHandle texture)
{
    RegisterFrameTextureImpl(rgTexture, texture);
}

namespace
{
/**
 * @brief Stops the run after a frame graph error when the validation gate is on.
 *
 * A graph that fails to compile or resolve drops its passes: the frame still
 * presents, so without this a broken scene pass looks like an empty viewport
 * and a smoke run stays green. Under UHE_VALIDATION_FATAL it is a defect, the
 * same as a validation error.
 */
void StopOnFrameGraphError()
{
    if (!Validation::FatalValidationEnabled())
        return;
    std::cerr << "UHE_VALIDATION_FATAL: frame graph error, stopping.\n";
    std::_Exit(1);
}
} // namespace

RGTextureHandle VulkanDevice::ImportSwapchain()
{
    const vk::Extent2D extent = m_SwapChain.GetExtent();
    // The acquired image starts Undefined UNLESS a legacy scene pass already
    // rendered into it this frame — BeginRenderPass's swapchain fallback leaves
    // it in Present (EndRenderPass emits ColorAttachment→Present), so the
    // import must start there or the compiler's entry barrier would assert the
    // wrong oldLayout. The compiler then emits the entry transition and the
    // ColorAttachment→Present exit as this pass's pre/post barriers.
    const ImageState swapInitialState = m_LegacySwapchainRendered ? ImageState::Present : ImageState::Undefined;
    RGTextureHandle swap = m_FrameGraph.ImportTexture("Swapchain", m_SwapchainGeneration, extent.width, extent.height,
                                                      1, swapInitialState, ImageState::Present);
    m_SwapchainRGHandle = swap; // executor registration keys on this exact slot
    return swap;
}

FGTexture::Desc VulkanDevice::GetBackbufferDesc()
{
    const vk::Extent2D extent = m_SwapChain.GetExtent();
    FGTexture::Desc desc;
    desc.Width = extent.width;
    desc.Height = extent.height;
    desc.Format = GetSwapchainFormat();
    return desc;
}

TextureHandle VulkanDevice::GetExportedTexture(const std::string& name)
{
    return m_FrameGraphBridge.ExportedTexture(m_Frontend, name);
}

void VulkanDevice::RegisterFrameBuffer(RGBufferHandle rgBuffer, BufferHandle buffer)
{
    if (!rgBuffer.IsValid() || buffer == nullptr)
        return;
    m_RenderGraphExecutor.RegisterBuffer(rgBuffer, reinterpret_cast<VulkanBuffer*>(buffer)->GetHandle());
}

void VulkanDevice::FlushFrontend()
{
    if (m_FrontendFlushed)
        return;
    m_FrontendFlushed = true;
    std::vector<std::string> errors;
    if (!m_FrameGraphBridge.Translate(m_Frontend, m_FrameGraph, errors))
    {
        // The frontend's passes are dropped; the rest of the frame still runs.
        for (const std::string& error : errors)
            UHE_CORE_ERROR("FrameGraph: {}", error);
        StopOnFrameGraphError();
    }
}

void VulkanDevice::EndFrontendFrame()
{
    m_Frontend.Reset();
    m_FrameGraphBridge.EndFrame();
    m_FrontendFlushed = false;
}

void VulkanDevice::RegisterFrameTextureImpl(RGTextureHandle rgTexture, TextureHandle texture)
{
    if (!rgTexture.IsValid() || texture == nullptr)
        return;
    auto* vulkanTexture = reinterpret_cast<VulkanTexture*>(texture);
    m_RenderGraphExecutor.RegisterTexture(rgTexture, vulkanTexture->GetImage(),
                                          vulkanTexture->GetImageView().operator*(), vulkanTexture->GetDesc().width,
                                          vulkanTexture->GetDesc().height, vulkanTexture->GetDesc().format);
}

void VulkanDevice::EndFrameGraph()
{
    if (!m_ExtensionCheck.SupportsDynamicRendering())
    {
        UHE_CORE_ERROR("EndFrameGraph: dynamic rendering is not supported on this device; skipping frame graph");
        StopOnFrameGraphError();
        m_FrameGraphFailed = true;
        m_FrameGraph.Reset();
        return;
    }
    // §14 steps 4–5 + M5 step 4: feature passes were declared during this
    // frame's layer OnUpdate calls (AimLab's scene pass, ...) and the ImGui
    // pass was declared by VulkanImGuiLayer::End — both land in m_FrameGraph
    // BEFORE this runs. Resetting here would silently erase them (the graph
    // would compile to an ImGui-only frame and the scene would vanish), so
    // the frame is compiled exactly as declared. Reset happens at the end of
    // this function (or on an error path).
    FlushFrontend(); // a frame without a UI pass still realizes the frontend
    // UHE_RENDERGRAPH_NO_CACHE=1 compiles every frame: the A/B for a suspected
    // cache bug, read once (the answer cannot change mid-run).
    static const bool s_NoCache = Validation::ParseEnabledFlag(std::getenv("UHE_RENDERGRAPH_NO_CACHE"));
    RGCompileResult compiled = s_NoCache ? m_FrameGraph.Compile() : m_FrameGraph.CompileCached();
    if (!compiled.Ok())
    {
        for (const RGValidationError& error : compiled.errors)
            UHE_CORE_ERROR("FrameGraph: {} ({})", error.message, error.passName);
        StopOnFrameGraphError();
        m_FrameGraphFailed = true;
        m_FrameGraph.Reset();
        return;
    }

    // Registration is per-frame (executor contract); the swapchain image is
    // bound to the slot the import actually allocated (BeginImGuiPass stored
    // it in m_SwapchainRGHandle), then feature textures follow.
    RegisterSwapchainResources();

    std::vector<std::string> resolveErrors;
    RGResolvedFrame resolved =
        m_RenderGraphExecutor.Resolve(compiled.frame, m_FrameGraph.GetBuilder().Passes(), resolveErrors);
    if (!resolveErrors.empty())
    {
        for (const std::string& error : resolveErrors)
            UHE_CORE_ERROR("FrameGraph resolve: {}", error);
        StopOnFrameGraphError();
        m_FrameGraphFailed = true;
        m_FrameGraph.Reset();
        return;
    }

    // ── Recording-order edges (single primary command buffer): graph passes
    // record into the SAME vk::CommandBuffer the device submits, so the
    // TaskGraph must never run two passes out of declaration order even when
    // the resource DAG calls them independent — job completion order would
    // scramble the buffer (e.g. ImGui recorded before the AimLab scene pass,
    // which draws into a different framebuffer). Chaining every pass to its
    // predecessor preserves the compiled order; §8.5 lifts this later by
    // giving each pass its own command buffer (per-thread pools).
    RGResolvedFrame& orderable = resolved;
    for (size_t slot = 1; slot < orderable.passes.size(); ++slot)
    {
        const u32 previous = static_cast<u32>(slot) - 1;
        auto& deps = orderable.passes[slot].dependencies;
        if (std::find(deps.begin(), deps.end(), previous) == deps.end())
            deps.push_back(previous);
    }

    m_PassNodes =
        m_RenderGraphExecutor.MapToTaskgraph(m_TaskGraph, resolved, m_FrameGraph.GetBuilder().Passes(),
                                             m_Frames[m_CurrentFrame].GetCommandBuffer().GetHandle(), m_BarrierEncoder);
    m_RenderGraphExecutor.ExecuteGraph(m_TaskGraph, m_Jobsystem, m_PassNodes);
    // Recorded: the exported textures end this frame in their final states.
    m_FrameGraphBridge.CommitExports();
    m_TaskGraph.Reset();
    m_FrameGraph.Reset();
}

void VulkanDevice::RegisterSwapchainResources()
{
    // §8.5: the device binds the live swapchain images to RG handles each
    // frame so the executor can resolve graph barriers/attachments. Handles
    // are RGTextureHandle{imageIndex, generation=1}: stable per image index
    // for the declaration API, and re-registered (not stale) after every
    // resize because registration is refreshed every Begin().
    const u32 imageCount = static_cast<u32>(m_SwapChain.GetImages().size());
    if (m_ImageIndex >= imageCount)
        return; // defensive: stale index after a surface loss
    if (!m_SwapchainRGHandle.IsValid())
        return; // no import declared this frame — nothing to bind
    const vk::Image image = m_SwapChain.GetImages()[m_ImageIndex];
    const TextureFormat swapFormat = ToEngineFormat(m_SwapChain.GetSurfaceFormat().format);
    // Key on the slot the import actually allocated, NOT the raw image index:
    // imported resources get sequential registry slots, which only coincide
    // with imageIndex when it happens to be 0.
    m_RenderGraphExecutor.RegisterTexture(m_SwapchainRGHandle, image, m_SwapChain.GetImageView(m_ImageIndex),
                                          m_SwapChain.GetExtent().width, m_SwapChain.GetExtent().height, swapFormat);
}

void VulkanDevice::ImmediateSubmit(std::function<void(vk::raii::CommandBuffer& cmd)>&& function)
{
    vk::CommandBufferAllocateInfo allocInfo{
        .commandPool = *m_UploadCommandPool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1};

    vk::raii::CommandBuffers cmdBuffers(m_LogicalDevice.getLogicalDevice(), allocInfo);
    vk::raii::CommandBuffer cmd = std::move(cmdBuffers[0]);

    vk::CommandBufferBeginInfo beginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
    cmd.begin(beginInfo);

    function(cmd);

    cmd.end();

    vk::SubmitInfo submitInfo{.commandBufferCount = 1, .pCommandBuffers = &(*cmd)};

    vk::raii::Queue& m_graphicsQueue = m_LogicalDevice.getGraphicsQueue();
    m_graphicsQueue.submit(submitInfo, *m_UploadFence);

    // Wait for the command to finish executing
    auto waitResult = m_LogicalDevice.getLogicalDevice().waitForFences({*m_UploadFence}, VK_TRUE, UINT64_MAX);
    UHE_CORE_ASSERT(waitResult == vk::Result::eSuccess, "Failed to wait for upload fence!");

    m_LogicalDevice.getLogicalDevice().resetFences({*m_UploadFence});
    m_UploadCommandPool.reset();
}

void VulkanDevice::WaitIdle()
{
    UHE_PROFILE_FUNCTION();
    getLogicalDevClass().getLogicalDevice().waitIdle();
}

void VulkanDevice::ResetCommandBuffers()
{
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        m_Frames[i].GetCommandBuffer().Reset();
    }
}

void VulkanDevice::ReadPixel(TextureHandle handle, int x, int y, void* outData)
{
    auto* texture = reinterpret_cast<VulkanTexture*>(handle);
    vk::Image image = texture->GetImage();

    CreatedBuffer readbackBuf =
        ::UHE::RHI::VULKAN::CreateBuffer(4, vk::BufferUsageFlagBits::eTransferDst, VMA_MEMORY_USAGE_GPU_TO_CPU);
    if (!readbackBuf.buffer)
    {
        UHE_CORE_ERROR("Failed to create readback buffer for ReadPixel!");
        return;
    }

    ImmediateSubmit(
        [&](vk::raii::CommandBuffer& cmd)
        {
            // The texel was last written as a colour attachment in an earlier
            // submission: wait for that stage, not for an earlier transfer.
            TransitionLayout(cmd, image, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                             vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead,
                             vk::AccessFlagBits::eTransferRead,
                             vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                 vk::PipelineStageFlagBits::eFragmentShader,
                             vk::PipelineStageFlagBits::eTransfer);

            vk::BufferImageCopy region{.bufferOffset = 0,
                                       .bufferRowLength = 0,
                                       .bufferImageHeight = 0,
                                       .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                            .mipLevel = 0,
                                                            .baseArrayLayer = 0,
                                                            .layerCount = 1},
                                       .imageOffset = vk::Offset3D{x, y, 0},
                                       .imageExtent = vk::Extent3D{1, 1, 1}};

            cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, readbackBuf.buffer, region);

            TransitionLayout(cmd, image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                             vk::AccessFlagBits::eTransferRead,
                             vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentWrite,
                             vk::PipelineStageFlagBits::eTransfer,
                             vk::PipelineStageFlagBits::eFragmentShader |
                                 vk::PipelineStageFlagBits::eColorAttachmentOutput);
        });

    void* mappedData = nullptr;
    VkResult res = vmaMapMemory(m_Allocator, readbackBuf.allocation, &mappedData);
    if (res == VK_SUCCESS && mappedData)
    {
        memcpy(outData, mappedData, 4);
        vmaUnmapMemory(m_Allocator, readbackBuf.allocation);
    }

    vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(readbackBuf.buffer), readbackBuf.allocation);
}

bool VulkanDevice::ReadPixelAsync(TextureHandle handle, int x, int y, void* outData)
{
    if (handle != nullptr && x >= 0 && y >= 0)
        m_PixelRequest = PixelRequest{.Texture = handle, .X = x, .Y = y};

    if (!m_LastPixel)
        return false;
    std::memcpy(outData, &*m_LastPixel, sizeof(u32));
    return true;
}

void VulkanDevice::RecordPixelReadback(vk::raii::CommandBuffer& cmd)
{
    if (!m_PixelRequest)
        return;

    PixelReadback& readback = m_PixelReadbacks[m_CurrentFrame];
    if (!readback.Buffer)
    {
        const CreatedBuffer created =
            ::UHE::RHI::VULKAN::CreateBuffer(4, vk::BufferUsageFlagBits::eTransferDst, VMA_MEMORY_USAGE_GPU_TO_CPU);
        if (!created.buffer)
        {
            UHE_CORE_ERROR("ReadPixelAsync: could not create the readback buffer");
            return;
        }
        readback.Buffer = created.buffer;
        readback.Allocation = created.allocation;
    }

    auto* texture = reinterpret_cast<VulkanTexture*>(m_PixelRequest->Texture);
    const auto& desc = texture->GetDesc();
    if (static_cast<u32>(m_PixelRequest->X) >= desc.width || static_cast<u32>(m_PixelRequest->Y) >= desc.height)
        return; // the texture was resized under the request
    const vk::Image image = texture->GetImage();

    // Recorded after the frame graph, so it sees this frame's scene pass. The
    // attachment ends the graph sampled by ImGui (ShaderReadOnly), as it does
    // for the blocking ReadPixel.
    TransitionLayout(cmd, image, vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
                     vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eShaderRead,
                     vk::AccessFlagBits::eTransferRead,
                     vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader,
                     vk::PipelineStageFlagBits::eTransfer);

    const vk::BufferImageCopy region{.bufferOffset = 0,
                                     .bufferRowLength = 0,
                                     .bufferImageHeight = 0,
                                     .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                          .mipLevel = 0,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1},
                                     .imageOffset = vk::Offset3D{m_PixelRequest->X, m_PixelRequest->Y, 0},
                                     .imageExtent = vk::Extent3D{1, 1, 1}};
    cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, readback.Buffer, region);

    TransitionLayout(cmd, image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::AccessFlagBits::eTransferRead,
                     vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eColorAttachmentWrite,
                     vk::PipelineStageFlagBits::eTransfer,
                     vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eColorAttachmentOutput);

    // Host visibility of the copy is covered by the submit's fence signal.
    readback.InFlight = true;
}

void VulkanDevice::CollectPixelReadback()
{
    PixelReadback& readback = m_PixelReadbacks[m_CurrentFrame];
    if (!readback.InFlight)
        return;
    readback.InFlight = false;

    void* mapped = nullptr;
    if (vmaMapMemory(m_Allocator, readback.Allocation, &mapped) == VK_SUCCESS && mapped)
    {
        vmaInvalidateAllocation(m_Allocator, readback.Allocation, 0, VK_WHOLE_SIZE);
        u32 value = 0;
        std::memcpy(&value, mapped, sizeof(value));
        m_LastPixel = value;
        vmaUnmapMemory(m_Allocator, readback.Allocation);
    }
}

bool VulkanDevice::RequestFrameCapture(std::function<void(const CapturedFrame&)> onCaptured)
{
    if (!m_SwapChain.SupportsTransferSrc())
    {
        UHE_CORE_ERROR("RequestFrameCapture: the surface does not allow reading swapchain images back");
        return false;
    }
    const vk::Format format = m_SwapChain.GetSurfaceFormat().format;
    const bool bgra = format == vk::Format::eB8G8R8A8Srgb || format == vk::Format::eB8G8R8A8Unorm;
    const bool rgba = format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eR8G8B8A8Unorm;
    if (!bgra && !rgba)
    {
        UHE_CORE_ERROR("RequestFrameCapture: unsupported swapchain format {0}", vk::to_string(format));
        return false;
    }
    m_FrameCapture.Callback = std::move(onCaptured);
    m_FrameCapture.SwapRedBlue = bgra;
    return true;
}

bool VulkanDevice::RecordFrameCapture(vk::raii::CommandBuffer& cmd)
{
    if (!m_FrameCapture.Callback)
        return false;

    const vk::Extent2D extent = m_SwapChain.GetExtent();
    const vk::DeviceSize size = static_cast<vk::DeviceSize>(extent.width) * extent.height * 4;
    const CreatedBuffer created =
        ::UHE::RHI::VULKAN::CreateBuffer(size, vk::BufferUsageFlagBits::eTransferDst, VMA_MEMORY_USAGE_GPU_TO_CPU);
    if (!created.buffer)
    {
        UHE_CORE_ERROR("RequestFrameCapture: could not create a {0}-byte readback buffer", size);
        m_FrameCapture.Callback = nullptr;
        return false;
    }
    m_FrameCapture.Buffer = created.buffer;
    m_FrameCapture.Allocation = created.allocation;
    m_FrameCapture.Width = extent.width;
    m_FrameCapture.Height = extent.height;

    // Recorded last: the graph (or the recovery path above) has already left
    // the image in PresentSrc with every colour write of the frame before it.
    const vk::Image image = m_SwapChain.GetImages()[m_ImageIndex];
    TransitionLayout(cmd, image, vk::ImageLayout::ePresentSrcKHR, vk::ImageLayout::eTransferSrcOptimal,
                     vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eTransferRead,
                     vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer);

    const vk::BufferImageCopy region{.bufferOffset = 0,
                                     .bufferRowLength = 0,
                                     .bufferImageHeight = 0,
                                     .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                          .mipLevel = 0,
                                                          .baseArrayLayer = 0,
                                                          .layerCount = 1},
                                     .imageOffset = vk::Offset3D{0, 0, 0},
                                     .imageExtent = vk::Extent3D{extent.width, extent.height, 1}};
    cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, m_FrameCapture.Buffer, region);

    TransitionLayout(cmd, image, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::ePresentSrcKHR,
                     vk::AccessFlagBits::eTransferRead, vk::AccessFlags{}, vk::PipelineStageFlagBits::eTransfer,
                     vk::PipelineStageFlagBits::eBottomOfPipe);
    return true;
}

void VulkanDevice::CollectFrameCapture()
{
    // Blocking by design: a capture is a test hook, and the callback wants the
    // pixels of this frame, not of one a few frames later.
    const auto waitResult = m_LogicalDevice.getLogicalDevice().waitForFences(
        {*m_Frames[m_CurrentFrame].GetInFlightFence()}, VK_TRUE, UINT64_MAX);

    CapturedFrame frame;
    void* mapped = nullptr;
    if (waitResult == vk::Result::eSuccess &&
        vmaMapMemory(m_Allocator, m_FrameCapture.Allocation, &mapped) == VK_SUCCESS && mapped)
    {
        vmaInvalidateAllocation(m_Allocator, m_FrameCapture.Allocation, 0, VK_WHOLE_SIZE);
        frame.Width = m_FrameCapture.Width;
        frame.Height = m_FrameCapture.Height;
        frame.Rgba.resize(static_cast<size_t>(frame.Width) * frame.Height * 4);
        std::memcpy(frame.Rgba.data(), mapped, frame.Rgba.size());
        vmaUnmapMemory(m_Allocator, m_FrameCapture.Allocation);
        if (m_FrameCapture.SwapRedBlue)
        {
            for (size_t i = 0; i < frame.Rgba.size(); i += 4)
                std::swap(frame.Rgba[i], frame.Rgba[i + 2]);
        }
    }
    else
    {
        UHE_CORE_ERROR("RequestFrameCapture: the captured frame could not be read back");
    }
    vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_FrameCapture.Buffer), m_FrameCapture.Allocation);

    // Cleared before the call, so the callback may request another capture.
    auto callback = std::move(m_FrameCapture.Callback);
    m_FrameCapture = {};
    if (!frame.Rgba.empty())
        callback(frame);
}

} // namespace UHE::RHI::VULKAN
