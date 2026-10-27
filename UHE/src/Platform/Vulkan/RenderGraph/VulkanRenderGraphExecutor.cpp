#include "uhepch.h"
#include "VulkanRenderGraphExecutor.h"
#include <cstring>
#include "Platform/Vulkan/VulkanBarrierEncoder.h"
#include "Platform/Vulkan/VulkanCommandBuffer.h"
#include "Platform/Vulkan/VulkanContext.h"

namespace UHE::RHI::VULKAN
{

const vk::detail::DispatchLoaderDynamic& RGPassContext::Dispatcher() const
{
    return VULKAN_HPP_DEFAULT_DISPATCHER;
}

namespace
{
// Barrier subresource aspects must match the image's format: depth/stencil
// images need DEPTH|STENCIL (separateDepthStencilLayouts is not enabled),
// everything else COLOR. Registration carries the engine format for exactly
// this kind of per-format decision.
vk::ImageAspectFlags AspectMaskFor(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::D24_UNORM_S8: // maps to VK_FORMAT_D32_SFLOAT_S8_UINT
            return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        case TextureFormat::D32_FLOAT: // maps to VK_FORMAT_D32_SFLOAT — depth only
            return vk::ImageAspectFlagBits::eDepth;
        default:
            return vk::ImageAspectFlagBits::eColor;
    }
}
} // namespace

// ─── Registration (owner: device, §14 step 4) ───────────────────────────────

void VulkanRenderGraphExecutor::RegisterTexture(RGTextureHandle texture, vk::Image image,
                                                vk::ImageView view, u32 width, u32 height,
                                                TextureFormat format)
{
    if (!texture.IsValid())
    {
        m_RegistrationWarnings.push_back("RegisterTexture: invalid handle");
        return;
    }
    if (image == nullptr || view == nullptr)
    {
        m_RegistrationWarnings.push_back("RegisterTexture: null image or view");
        return;
    }
    m_Textures[texture.index] = RGRegisteredTexture{image, view, width, height, format, texture.generation};
}

void VulkanRenderGraphExecutor::RegisterBuffer(RGBufferHandle buffer, vk::Buffer vkBuffer)
{
    if (!buffer.IsValid() || vkBuffer == nullptr)
    {
        m_RegistrationWarnings.push_back("RegisterBuffer: invalid handle or null buffer");
        return;
    }
    m_Buffers[buffer.index] = RGRegisteredBuffer{vkBuffer, buffer.generation};
}

void VulkanRenderGraphExecutor::ClearRegistrations()
{
    m_Textures.clear();
    m_Buffers.clear();
    m_RegistrationWarnings.clear();
}

// ─── Resolve ────────────────────────────────────────────────────────────────

RGResolvedFrame VulkanRenderGraphExecutor::Resolve(const RGCompiledFrame& compiled,
                                                   const std::vector<RGPassSpec>& specs,
                                                   std::vector<std::string>& errors) const
{
    RGResolvedFrame resolved;
    if (!errors.empty())
        return resolved; // caller is already failing; keep the frame empty

    const auto fail = [&](const std::string& message) { errors.push_back(message); };

    for (const RGCompiledPass& compiledPass : compiled.passes)
    {
        const RGPassSpec& spec = specs[compiledPass.passIndex];

        RGResolvedPass pass;
        pass.compiledId = compiledPass.id;
        pass.passIndex = compiledPass.passIndex;
        pass.name = compiledPass.name;
        pass.type = compiledPass.type;
        pass.dependencies = compiledPass.dependencies;

        // Attachments resolve first: they define the rendering scope.
        if (spec.type == RGPassType::Graphics)
        {
            for (const RGColorAttachment& color : spec.colors)
            {
                const auto it = m_Textures.find(color.texture.index);
                if (it == m_Textures.end() || it->second.generation != color.texture.generation)
                {
                    fail(pass.name + ": color attachment \"" + "texture#" +
                         std::to_string(color.texture.index) + "\" is not registered");
                    continue;
                }
                const RGRegisteredTexture& reg = it->second;
                RGResolvedColorAttachment resolvedColor;
                resolvedColor.view = reg.view;
                resolvedColor.load = color.load;
                resolvedColor.store = color.store;
                if (reg.format == TextureFormat::R32_SINT)
                {
                    // Integer target: the clear value is an int (legacy
                    // BeginRenderPass semantics — entity-ID targets clear to
                    // the float's truncation, e.g. -1).
                    resolvedColor.clearColor = vk::ClearColorValue(std::array<int32_t, 4>{
                        static_cast<int32_t>(color.clearColor[0]), 0, 0, 0});
                }
                else
                {
                    resolvedColor.clearColor = vk::ClearColorValue(
                        std::array<float, 4>{color.clearColor[0], color.clearColor[1],
                                             color.clearColor[2], color.clearColor[3]});
                }
                pass.colorAttachments.push_back(std::move(resolvedColor));
                pass.renderExtent = vk::Extent2D{reg.width, reg.height};
            }

            if (spec.hasDepth && spec.depth.texture.IsValid())
            {
                const auto it = m_Textures.find(spec.depth.texture.index);
                if (it == m_Textures.end() || it->second.generation != spec.depth.texture.generation)
                {
                    fail(pass.name + ": depth attachment is not registered");
                }
                else
                {
                    const RGRegisteredTexture& reg = it->second;
                    RGResolvedDepthAttachment resolvedDepth;
                    resolvedDepth.view = reg.view;
                    resolvedDepth.load = spec.depth.load;
                    resolvedDepth.store = spec.depth.store;
                    resolvedDepth.clear = vk::ClearDepthStencilValue{spec.depth.clearDepth,
                                                                     spec.depth.clearStencil};
                    pass.depthAttachment = std::move(resolvedDepth);
                    if (!pass.renderExtent)
                        pass.renderExtent = vk::Extent2D{reg.width, reg.height};
                }
            }
        }

        // Pre-barriers: images need handle → vk::Image + full subresource range.
        for (const RGCompiledImageBarrier& barrier : compiledPass.preImageBarriers)
        {
            const auto it = m_Textures.find(barrier.texture.index);
            if (it == m_Textures.end() || it->second.generation != barrier.texture.generation)
            {
                fail(pass.name + ": pre-barrier texture#" + std::to_string(barrier.texture.index) +
                     " is not registered");
                continue;
            }
            const RGRegisteredTexture& reg = it->second;
            ImageBarrier resolvedBarrier;
            resolvedBarrier.Old = barrier.oldState;
            resolvedBarrier.Next = barrier.newState;
            resolvedBarrier.SrcStage = barrier.srcStage;
            resolvedBarrier.DstStage = barrier.dstStage;
            resolvedBarrier.SrcAccess = barrier.srcAccess;
            resolvedBarrier.DstAccess = barrier.dstAccess;
            resolvedBarrier.Image = reg.image;
            resolvedBarrier.Range =
                vk::ImageSubresourceRange{AspectMaskFor(reg.format), 0, VK_REMAINING_MIP_LEVELS,
                                          0, VK_REMAINING_ARRAY_LAYERS};
            pass.preImageBarriers.push_back({resolvedBarrier});
        }

        for (const RGCompiledImageBarrier& barrier : compiledPass.postImageBarriers)
        {
            const auto it = m_Textures.find(barrier.texture.index);
            if (it == m_Textures.end() || it->second.generation != barrier.texture.generation)
            {
                fail(pass.name + ": post-barrier texture#" + std::to_string(barrier.texture.index) +
                     " is not registered");
                continue;
            }
            const RGRegisteredTexture& reg = it->second;
            ImageBarrier resolvedBarrier;
            resolvedBarrier.Old = barrier.oldState;
            resolvedBarrier.Next = barrier.newState;
            resolvedBarrier.SrcStage = barrier.srcStage;
            resolvedBarrier.DstStage = barrier.dstStage;
            resolvedBarrier.SrcAccess = barrier.srcAccess;
            resolvedBarrier.DstAccess = barrier.dstAccess;
            resolvedBarrier.Image = reg.image;
            resolvedBarrier.Range =
                vk::ImageSubresourceRange{AspectMaskFor(reg.format), 0, VK_REMAINING_MIP_LEVELS,
                                          0, VK_REMAINING_ARRAY_LAYERS};
            pass.postImageBarriers.push_back({resolvedBarrier});
        }

        for (const RGCompiledBufferBarrier& barrier : compiledPass.preBufferBarriers)
        {
            const auto it = m_Buffers.find(barrier.buffer.index);
            if (it == m_Buffers.end() || it->second.generation != barrier.buffer.generation)
            {
                fail(pass.name + ": buffer barrier buffer#" + std::to_string(barrier.buffer.index) +
                     " is not registered");
                continue;
            }
            BufferBarrier resolvedBarrier;
            resolvedBarrier.Buffer = it->second.buffer;
            resolvedBarrier.SrcStage = barrier.srcStage;
            resolvedBarrier.DstStage = barrier.dstStage;
            resolvedBarrier.SrcAccess = barrier.srcAccess;
            resolvedBarrier.DstAccess = barrier.dstAccess;
            pass.preBufferBarriers.push_back({resolvedBarrier});
        }

        resolved.passes.push_back(std::move(pass));
    }

    resolved.topologyHash = compiled.topologyHash;
    return resolved;
}

// ─── Record ─────────────────────────────────────────────────────────────────

void VulkanRenderGraphExecutor::Record(const RGResolvedFrame& resolved,
                                       const std::vector<RGPassSpec>& specs,
                                       VulkanCommandBuffer& target, VulkanBarrierEncoder& encoder,
                                       std::vector<std::string>& errors) const
{
    UHE_PROFILE_FUNCTION();
    RecordRange(resolved, specs, 0, static_cast<u32>(resolved.passes.size()),
                target.GetHandle(), encoder, errors);
}

void VulkanRenderGraphExecutor::RecordRange(const RGResolvedFrame& resolved,
                                            const std::vector<RGPassSpec>& specs, u32 firstSlot,
                                            u32 endSlot, vk::CommandBuffer cmd,
                                            VulkanBarrierEncoder& encoder,
                                            std::vector<std::string>& errors) const
{
    (void)errors; // Resolve reports missing registrations; Record skips them

    endSlot = std::min(endSlot, static_cast<u32>(resolved.passes.size()));

    for (u32 slot = firstSlot; slot < endSlot; ++slot)
    {
        const RGResolvedPass& pass = resolved.passes[slot];
        // ── Pre-barriers (tier-aware encoding happens inside the encoder) ──
        std::vector<ImageBarrier> images;
        images.reserve(pass.preImageBarriers.size());
        for (const RGResolvedImageBarrier& barrier : pass.preImageBarriers)
            images.push_back(barrier.barrier);

        std::vector<BufferBarrier> buffers;
        buffers.reserve(pass.preBufferBarriers.size());
        for (const RGResolvedBufferBarrier& barrier : pass.preBufferBarriers)
            buffers.push_back(barrier.barrier);

        encoder.Encode(cmd, images, buffers, /*globalBarrier=*/nullptr);

        // ── Dynamic-rendering scope for graphics passes ──
        const RGPassSpec& spec = specs[pass.passIndex];
        const bool graphicsScope = pass.type == RGPassType::Graphics && pass.renderExtent.has_value();
        if (graphicsScope)
        {
            std::vector<vk::RenderingAttachmentInfo> colorInfos;
            colorInfos.reserve(pass.colorAttachments.size());
            for (const RGResolvedColorAttachment& color : pass.colorAttachments)
            {
                colorInfos.push_back({
                    .imageView = color.view,
                    .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .loadOp = ToVkLoadOp(color.load),
                    .storeOp = ToVkStoreOp(color.store),
                    .clearValue = color.clearColor,
                });
            }

            vk::RenderingAttachmentInfo depthInfo{};
            bool hasDepth = false;
            if (pass.depthAttachment)
            {
                depthInfo.imageView = pass.depthAttachment->view;
                depthInfo.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
                depthInfo.loadOp = ToVkLoadOp(pass.depthAttachment->load);
                depthInfo.storeOp = ToVkStoreOp(pass.depthAttachment->store);
                depthInfo.clearValue = pass.depthAttachment->clear;
                hasDepth = true;
            }

            const vk::RenderingInfo renderingInfo{
                .renderArea = vk::Rect2D{vk::Offset2D{0, 0}, *pass.renderExtent},
                .layerCount = 1,
                .colorAttachmentCount = static_cast<u32>(colorInfos.size()),
                .pColorAttachments = colorInfos.data(),
                .pDepthAttachment = hasDepth ? &depthInfo : nullptr,
            };
            cmd.beginRendering(renderingInfo);
        }

        // ── The pass's own recording (via the §8.4 context surface) ──
        if (spec.execute)
        {
            RGPassContext context(*this, cmd, pass.passIndex,
                                  graphicsScope ? *pass.renderExtent : vk::Extent2D{});
            spec.execute(context);
        }

        if (graphicsScope)
            cmd.endRendering();

        // ── Post-barriers (finalState exits) ──
        if (!pass.postImageBarriers.empty())
        {
            std::vector<ImageBarrier> postImages;
            postImages.reserve(pass.postImageBarriers.size());
            for (const RGResolvedImageBarrier& barrier : pass.postImageBarriers)
                postImages.push_back(barrier.barrier);
            encoder.Encode(cmd, postImages, {}, nullptr);
            postImages.clear();
        }
    }
}

// ─── Submit ─────────────────────────────────────────────────────────────────

void VulkanRenderGraphExecutor::Submit(VulkanContext* context, vk::raii::Queue& queue,
                                       vk::CommandBuffer cmd, std::span<const SemaphoreWait> waits,
                                       std::span<const SemaphoreSignal> signals, vk::Fence fence)
{
    const SubmitInfo info{.Waits = waits, .CommandBuffers = std::span(&cmd, 1), .Signals = signals};
    VulkanSemaphore::Submit(context, queue, info, fence);
}

// ─── TaskGraph mapping (§8.5, §14 step 5) ───────────────────────────────

std::vector<Jobsystem::TaskID> VulkanRenderGraphExecutor::MapToTaskgraph(
    Jobsystem::TaskGraph& graph, const RGResolvedFrame& resolved,
    const std::vector<RGPassSpec>& specs, vk::CommandBuffer targetCmd,
    VulkanBarrierEncoder& encoder) const
{
    // Node context: POD view over one resolved pass, stored in the executor's
    // m_NodeContexts deque (stable addresses, no per-frame heap churn).
    using NodeContext = VulkanRenderGraphExecutor::NodeContext;
    static_assert(std::is_trivially_copyable_v<NodeContext>);
    m_NodeContexts.clear();

    std::vector<Jobsystem::TaskID> nodes;
    nodes.reserve(resolved.passes.size());
    for (u32 slot = 0; slot < resolved.passes.size(); ++slot)
    {
        NodeContext& stored = m_NodeContexts.emplace_back(
            NodeContext{this, &resolved, &specs, targetCmd, &encoder, slot});
        NodeContext* context = &stored;
        const auto entry = [](void* p)
        {
            const NodeContext* context = static_cast<const NodeContext*>(p);
            // One pass's recording (barriers + scope + execute + post). Recording
            // pass i+1 while the device submits pass i is the §8.5 pipelining shape.
            std::vector<std::string> errors;
            context->executor->RecordRange(*context->resolved, *context->specs,
                                           context->passSlot, context->passSlot + 1,
                                           context->targetCmd, *context->encoder, errors);
        };
        nodes.push_back(graph.CreateTask(entry, context));
    }

    // Edges: compiled DAG — a pass runs after all its dependencies (§8.5).
    for (u32 slot = 0; slot < resolved.passes.size(); ++slot)
    {
        const RGResolvedPass& pass = resolved.passes[slot];
        for (const u32 depId : pass.dependencies)
            graph.AddDependency(nodes[depId], nodes[slot]);
    }
    return nodes;
}

void VulkanRenderGraphExecutor::ExecuteGraph(Jobsystem::TaskGraph& graph, Jobsystem::UheJobsystem& jobs,
                                             std::span<const Jobsystem::TaskID> nodes)
{
    if (nodes.empty())
    {
        m_NodeContexts.clear();
        return;
    }
    graph.Execute(jobs);
    // Node contexts live in m_NodeContexts (member deque); safe to reclaim now
    // that every job has completed.
    m_NodeContexts.clear();
}

// ─── RGPassContext resolution ───────────────────────────────────────────────

vk::ImageView VulkanRenderGraphExecutor::ResolveView(RGTextureHandle texture) const
{
    const auto it = m_Textures.find(texture.index);
    return it != m_Textures.end() && it->second.generation == texture.generation ? it->second.view
                                                                                 : vk::ImageView{nullptr};
}

vk::Buffer VulkanRenderGraphExecutor::ResolveBuffer(RGBufferHandle buffer) const
{
    const auto it = m_Buffers.find(buffer.index);
    return it != m_Buffers.end() && it->second.generation == buffer.generation ? it->second.buffer
                                                                               : vk::Buffer{nullptr};
}

vk::ImageView RGPassContext::View(RGTextureHandle texture) const
{
    return m_Executor.ResolveView(texture);
}

vk::Buffer RGPassContext::Buffer(RGBufferHandle buffer) const
{
    return m_Executor.ResolveBuffer(buffer);
}

vk::Extent2D VulkanRenderGraphExecutor::ResolveExtent(RGTextureHandle texture) const
{
    const auto it = m_Textures.find(texture.index);
    return it != m_Textures.end() && it->second.generation == texture.generation
               ? vk::Extent2D{it->second.width, it->second.height}
               : vk::Extent2D{};
}

} // namespace UHE::RHI::VULKAN
