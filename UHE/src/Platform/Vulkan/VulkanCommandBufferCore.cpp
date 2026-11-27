// The RHI thin core on a Vulkan command buffer (issue #30, RHICore.h).
//
// Nothing here places a barrier or binds a set on its own: Barrier() is the
// only state change, BeginRendering() expects its attachments already in
// their attachment state, and copies expect TransferSrc/TransferDst. The
// convenience calls in VulkanCommandBuffer.cpp keep doing that for callers
// that do not track state.

#include "uhepch.h"
#include <algorithm>
#include "Platform/Vulkan/VulkanBarrierEncoder.h"
#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanCommandBuffer.h"
#include "Platform/Vulkan/VulkanCoreObjects.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanExtensionCheck.h"
#include "Platform/Vulkan/VulkanTexture.h"

namespace UHE::RHI::VULKAN
{
namespace
{
VulkanTexture* Tex(TextureHandle handle)
{
    return reinterpret_cast<VulkanTexture*>(handle);
}

vk::Buffer Buf(BufferHandle handle)
{
    return reinterpret_cast<VulkanBuffer*>(handle)->GetHandle();
}

vk::ImageSubresourceLayers Layers(const VulkanTexture& texture, const TextureRegion& region)
{
    const vk::ImageSubresourceRange range = texture.GetRange(region.Mip, 1, region.BaseLayer, region.LayerCount);
    // A copy names one aspect; depth-stencil copies the depth.
    const vk::ImageAspectFlags aspect =
        (range.aspectMask & vk::ImageAspectFlagBits::eDepth) ? vk::ImageAspectFlagBits::eDepth : range.aspectMask;
    return vk::ImageSubresourceLayers{.aspectMask = aspect,
                                      .mipLevel = range.baseMipLevel,
                                      .baseArrayLayer = range.baseArrayLayer,
                                      .layerCount = range.layerCount};
}

vk::Extent3D RegionExtent(const VulkanTexture& texture, const TextureRegion& region)
{
    const u32 width = std::max(texture.GetWidth() >> region.Mip, 1u);
    const u32 height = std::max(texture.GetHeight() >> region.Mip, 1u);
    return vk::Extent3D{region.Width != 0 ? region.Width : width, region.Height != 0 ? region.Height : height, 1};
}

vk::Offset3D RegionCorner(const TextureRegion& region, const vk::Extent3D& extent)
{
    return vk::Offset3D{region.X + static_cast<i32>(extent.width), region.Y + static_cast<i32>(extent.height), 1};
}

vk::BufferImageCopy BufferImage(const VulkanTexture& texture, const BufferTextureCopy& copy)
{
    return vk::BufferImageCopy{.bufferOffset = copy.BufferOffset,
                               .bufferRowLength = copy.RowLength,
                               .bufferImageHeight = 0,
                               .imageSubresource = Layers(texture, copy.Region),
                               .imageOffset = vk::Offset3D{copy.Region.X, copy.Region.Y, 0},
                               .imageExtent = RegionExtent(texture, copy.Region)};
}
} // namespace

void VulkanCommandBuffer::Barrier(std::span<const TextureBarrier> textures, std::span<const RHI::BufferBarrier> buffers,
                                  const MemoryDependency* memory)
{
    std::vector<ImageBarrier> images;
    images.reserve(textures.size());
    for (const TextureBarrier& t : textures)
    {
        const VulkanTexture* texture = Tex(t.Texture);
        if (texture == nullptr)
            continue;
        images.push_back(ImageBarrier{.Image = const_cast<VulkanTexture*>(texture)->GetImage(),
                                      .Range = texture->GetRange(t.BaseMip, t.MipCount, t.BaseLayer, t.LayerCount),
                                      .Old = t.Old,
                                      .Next = t.New,
                                      .SrcStage = t.SrcStage,
                                      .DstStage = t.DstStage,
                                      .SrcAccess = t.SrcAccess,
                                      .DstAccess = t.DstAccess});
    }
    std::vector<VULKAN::BufferBarrier> ranges;
    ranges.reserve(buffers.size());
    for (const RHI::BufferBarrier& b : buffers)
    {
        if (b.Buffer == nullptr)
            continue;
        ranges.push_back(VULKAN::BufferBarrier{.Buffer = Buf(b.Buffer),
                                               .Offset = b.Offset,
                                               .Size = b.Size == kWholeSize ? VK_WHOLE_SIZE : b.Size,
                                               .SrcStage = b.SrcStage,
                                               .DstStage = b.DstStage,
                                               .SrcAccess = b.SrcAccess,
                                               .DstAccess = b.DstAccess});
    }
    GlobalBarrier global;
    if (memory != nullptr)
        global = GlobalBarrier{.SrcStage = memory->SrcStage,
                               .DstStage = memory->DstStage,
                               .SrcAccess = memory->SrcAccess,
                               .DstAccess = memory->DstAccess};
    m_ctx->device->GetBarrierEncoder().Encode(*m_CommandBuffer, images, ranges, memory != nullptr ? &global : nullptr);
}

void VulkanCommandBuffer::BeginRendering(const RenderPassDesc& desc)
{
    std::array<vk::RenderingAttachmentInfo, 8> colors{};
    const u32 count = std::min<u32>(desc.colorAttachmentCount, 8);
    for (u32 i = 0; i < count; ++i)
    {
        const ColorAttachment& attachment = desc.colorAttachments[i];
        const glm::vec4& c = attachment.clearColor;
        // The texture's own format decides the clear type: an entity-ID target
        // clears as integers even when the attachment left its format at the default.
        vk::ClearValue clear =
            Tex(attachment.texture)->GetDesc().format == TextureFormat::R32_SINT
                ? vk::ClearValue{vk::ClearColorValue{std::array<i32, 4>{static_cast<i32>(c.r), static_cast<i32>(c.g),
                                                                        static_cast<i32>(c.b), static_cast<i32>(c.a)}}}
                : vk::ClearValue{vk::ClearColorValue{std::array<f32, 4>{c.r, c.g, c.b, c.a}}};
        colors[i] = vk::RenderingAttachmentInfo{.imageView = *Tex(attachment.texture)->GetImageView(),
                                                .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                                                .loadOp = ToVkLoadOp(attachment.loadOp),
                                                .storeOp = ToVkStoreOp(attachment.storeOp),
                                                .clearValue = clear};
    }
    vk::RenderingAttachmentInfo depth{};
    const bool hasDepth = desc.hasDepth && desc.depthAttachment.texture != nullptr;
    if (hasDepth)
        depth = vk::RenderingAttachmentInfo{
            .imageView = *Tex(desc.depthAttachment.texture)->GetImageView(),
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp = ToVkLoadOp(desc.depthAttachment.loadOp),
            .storeOp = ToVkStoreOp(desc.depthAttachment.storeOp),
            .clearValue = vk::ClearValue{vk::ClearDepthStencilValue{.depth = desc.depthAttachment.clearDepth,
                                                                    .stencil = desc.depthAttachment.clearStencil}}};
    const bool stencil =
        hasDepth && (Tex(desc.depthAttachment.texture)->GetAspect() & vk::ImageAspectFlagBits::eStencil);
    vk::RenderingInfo info{.flags = {},
                           .renderArea = {.offset = {0, 0}, .extent = {desc.renderWidth, desc.renderHeight}},
                           .layerCount = 1,
                           .viewMask = 0,
                           .colorAttachmentCount = count,
                           .pColorAttachments = count > 0 ? colors.data() : nullptr,
                           .pDepthAttachment = hasDepth ? &depth : nullptr,
                           .pStencilAttachment = stencil ? &depth : nullptr};
    m_CommandBuffer.beginRendering(info);
}

void VulkanCommandBuffer::EndRendering()
{
    m_CommandBuffer.endRendering();
}

void VulkanCommandBuffer::ExecuteSecondary(std::span<RHICommandBuffer* const> lists)
{
    std::vector<vk::CommandBuffer> handles;
    handles.reserve(lists.size());
    for (RHICommandBuffer* list : lists)
        if (auto* secondary = static_cast<VulkanCommandBuffer*>(list); secondary != nullptr)
        {
            secondary->End();
            handles.push_back(*secondary->GetHandle());
        }
    if (!handles.empty())
        m_CommandBuffer.executeCommands(handles);
}

void VulkanCommandBuffer::CopyBuffer(BufferHandle src, BufferHandle dst, const BufferCopy& region)
{
    m_CommandBuffer.copyBuffer(
        Buf(src), Buf(dst),
        vk::BufferCopy{.srcOffset = region.SrcOffset, .dstOffset = region.DstOffset, .size = region.Size});
}

void VulkanCommandBuffer::CopyBufferToTexture(BufferHandle src, TextureHandle dst, const BufferTextureCopy& region)
{
    VulkanTexture& texture = *Tex(dst);
    m_CommandBuffer.copyBufferToImage(Buf(src), texture.GetImage(), vk::ImageLayout::eTransferDstOptimal,
                                      BufferImage(texture, region));
}

void VulkanCommandBuffer::CopyTextureToBuffer(TextureHandle src, BufferHandle dst, const BufferTextureCopy& region)
{
    VulkanTexture& texture = *Tex(src);
    m_CommandBuffer.copyImageToBuffer(texture.GetImage(), vk::ImageLayout::eTransferSrcOptimal, Buf(dst),
                                      BufferImage(texture, region));
}

void VulkanCommandBuffer::CopyTexture(TextureHandle src, TextureHandle dst, const TextureCopy& region)
{
    VulkanTexture& from = *Tex(src);
    VulkanTexture& to = *Tex(dst);
    m_CommandBuffer.copyImage(from.GetImage(), vk::ImageLayout::eTransferSrcOptimal, to.GetImage(),
                              vk::ImageLayout::eTransferDstOptimal,
                              vk::ImageCopy{.srcSubresource = Layers(from, region.Src),
                                            .srcOffset = {region.Src.X, region.Src.Y, 0},
                                            .dstSubresource = Layers(to, region.Dst),
                                            .dstOffset = {region.Dst.X, region.Dst.Y, 0},
                                            .extent = RegionExtent(from, region.Src)});
}

void VulkanCommandBuffer::BlitTexture(TextureHandle src, TextureHandle dst, const TextureBlit& region,
                                      BlitFilter filter)
{
    VulkanTexture& from = *Tex(src);
    VulkanTexture& to = *Tex(dst);
    const vk::Extent3D srcExtent = RegionExtent(from, region.Src);
    const vk::Extent3D dstExtent = RegionExtent(to, region.Dst);
    const vk::ImageBlit blit{.srcSubresource = Layers(from, region.Src),
                             .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{region.Src.X, region.Src.Y, 0},
                                                                       RegionCorner(region.Src, srcExtent)},
                             .dstSubresource = Layers(to, region.Dst),
                             .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{region.Dst.X, region.Dst.Y, 0},
                                                                       RegionCorner(region.Dst, dstExtent)}};
    m_CommandBuffer.blitImage(from.GetImage(), vk::ImageLayout::eTransferSrcOptimal, to.GetImage(),
                              vk::ImageLayout::eTransferDstOptimal, blit,
                              filter == BlitFilter::Linear ? vk::Filter::eLinear : vk::Filter::eNearest);
}

void VulkanCommandBuffer::ResolveTexture(TextureHandle src, TextureHandle dst)
{
    VulkanTexture& from = *Tex(src);
    VulkanTexture& to = *Tex(dst);
    const TextureRegion whole{};
    m_CommandBuffer.resolveImage(from.GetImage(), vk::ImageLayout::eTransferSrcOptimal, to.GetImage(),
                                 vk::ImageLayout::eTransferDstOptimal,
                                 vk::ImageResolve{.srcSubresource = Layers(from, whole),
                                                  .srcOffset = {0, 0, 0},
                                                  .dstSubresource = Layers(to, whole),
                                                  .dstOffset = {0, 0, 0},
                                                  .extent = RegionExtent(from, whole)});
}

void VulkanCommandBuffer::DrawIndirect(BufferHandle args, u64 offset, u32 drawCount, u32 stride)
{
    m_CommandBuffer.drawIndirect(Buf(args), offset, drawCount, stride);
}

void VulkanCommandBuffer::DrawIndexedIndirect(BufferHandle args, u64 offset, u32 drawCount, u32 stride)
{
    m_CommandBuffer.drawIndexedIndirect(Buf(args), offset, drawCount, stride);
}

void VulkanCommandBuffer::DispatchIndirect(BufferHandle args, u64 offset)
{
    m_CommandBuffer.dispatchIndirect(Buf(args), offset);
}

void VulkanCommandBuffer::ResetQueries(QueryPoolHandle pool, u32 first, u32 count)
{
    m_CommandBuffer.resetQueryPool(*reinterpret_cast<VulkanCoreQueryPool*>(pool)->Pool, first, count);
}

void VulkanCommandBuffer::WriteTimestamp(QueryPoolHandle pool, u32 index, Stage stage)
{
    const vk::QueryPool queries = *reinterpret_cast<VulkanCoreQueryPool*>(pool)->Pool;
    if (m_ctx->CheckExtensions->SupportsSync2())
        m_CommandBuffer.writeTimestamp2(ToVkPipelineStage2(stage), queries, index);
    else
        m_CommandBuffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, queries, index);
}

void VulkanCommandBuffer::BeginQuery(QueryPoolHandle pool, u32 index)
{
    m_CommandBuffer.beginQuery(*reinterpret_cast<VulkanCoreQueryPool*>(pool)->Pool, index, {});
}

void VulkanCommandBuffer::EndQuery(QueryPoolHandle pool, u32 index)
{
    m_CommandBuffer.endQuery(*reinterpret_cast<VulkanCoreQueryPool*>(pool)->Pool, index);
}

void VulkanCommandBuffer::BindDescriptorSet(u32 set, DescriptorSetHandle descriptorSet)
{
    UHE_CORE_ASSERT(set >= 1 && set <= 3, "BindDescriptorSet: sets 1..3; set 0 is the bindless set");
    UHE_CORE_ASSERT(m_CurrentPipelineLayout, "BindDescriptorSet: bind a pipeline first");
    const auto* object = reinterpret_cast<VulkanCoreDescriptorSet*>(descriptorSet);
    m_CommandBuffer.bindDescriptorSets(m_CurrentBindPoint, m_CurrentPipelineLayout, set, {object->Set}, nullptr);
}

void VulkanCommandBuffer::PushDescriptors(u32 set, std::span<const DescriptorWrite> writes)
{
    UHE_CORE_ASSERT(set >= 1 && set <= 3, "PushDescriptors: sets 1..3; set 0 is the bindless set");
    UHE_CORE_ASSERT(m_CurrentPipelineLayout, "PushDescriptors: bind a pipeline first");
    UHE_CORE_ASSERT(m_ctx->CheckExtensions->Supports(Extension::PushDescriptor),
                    "PushDescriptors: the device has no VK_KHR_push_descriptor (DeviceLimits::PushDescriptors)");
    const VulkanDescriptorWrites vkWrites(nullptr, writes);
    m_CommandBuffer.pushDescriptorSetKHR(m_CurrentBindPoint, m_CurrentPipelineLayout, set, vkWrites.Writes);
}

void* VulkanCommandBuffer::GetNativeHandle() const
{
    return static_cast<VkCommandBuffer>(*m_CommandBuffer);
}

void VulkanCommandBuffer::BeginList(const CommandListDesc& desc)
{
    m_Queue = desc.Queue;
    m_IsSecondary = desc.Level == CommandListLevel::Secondary;
    m_CurrentPipelineLayout = nullptr;
    if (!m_IsSecondary)
    {
        m_CommandBuffer.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        return;
    }
    // A secondary list drawn inside a rendering scope inherits its formats.
    std::array<vk::Format, 8> formats{};
    const u32 count = std::min<u32>(desc.ColorFormatCount, 8);
    for (u32 i = 0; i < count; ++i)
        formats[i] = MapTextureFormat(desc.ColorFormats[i]);
    const vk::Format depth =
        desc.DepthFormat == TextureFormat::Undefined ? vk::Format::eUndefined : MapTextureFormat(desc.DepthFormat);
    const bool stencil = depth == vk::Format::eD24UnormS8Uint || depth == vk::Format::eD32SfloatS8Uint;
    vk::CommandBufferInheritanceRenderingInfo rendering{.flags = {},
                                                        .viewMask = 0,
                                                        .colorAttachmentCount = count,
                                                        .pColorAttachmentFormats = formats.data(),
                                                        .depthAttachmentFormat = depth,
                                                        .stencilAttachmentFormat =
                                                            stencil ? depth : vk::Format::eUndefined,
                                                        .rasterizationSamples = vk::SampleCountFlagBits::e1};
    vk::CommandBufferInheritanceInfo inheritance{.pNext = desc.InsideRendering ? &rendering : nullptr};
    vk::CommandBufferUsageFlags flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (desc.InsideRendering)
        flags |= vk::CommandBufferUsageFlagBits::eRenderPassContinue;
    m_CommandBuffer.begin(vk::CommandBufferBeginInfo{.flags = flags, .pInheritanceInfo = &inheritance});
}

// ── Descriptor helpers shared with the device ────────────────────────────────

vk::DescriptorType ToVkDescriptorType(DescriptorType type)
{
    switch (type)
    {
        case DescriptorType::UniformBuffer:
            return vk::DescriptorType::eUniformBuffer;
        case DescriptorType::StorageBuffer:
            return vk::DescriptorType::eStorageBuffer;
        case DescriptorType::SampledTexture:
            return vk::DescriptorType::eCombinedImageSampler;
        case DescriptorType::StorageTexture:
            return vk::DescriptorType::eStorageImage;
    }
    return vk::DescriptorType::eStorageBuffer;
}

vk::ShaderStageFlags ToVkShaderStages(ShaderStage stages)
{
    switch (stages)
    {
        case ShaderStage::Vertex:
            return vk::ShaderStageFlagBits::eVertex;
        case ShaderStage::Fragment:
            return vk::ShaderStageFlagBits::eFragment;
        case ShaderStage::Compute:
            return vk::ShaderStageFlagBits::eCompute;
        case ShaderStage::AllGraphics:
            return vk::ShaderStageFlagBits::eAllGraphics;
    }
    return vk::ShaderStageFlagBits::eAll;
}

VulkanDescriptorWrites::VulkanDescriptorWrites(vk::DescriptorSet set, std::span<const DescriptorWrite> writes)
{
    // Reserved up front: the writes point into these vectors.
    Buffers.reserve(writes.size());
    Images.reserve(writes.size());
    Writes.reserve(writes.size());
    for (const DescriptorWrite& w : writes)
    {
        vk::WriteDescriptorSet write{.dstSet = set,
                                     .dstBinding = w.Binding,
                                     .dstArrayElement = w.ArrayElement,
                                     .descriptorCount = 1,
                                     .descriptorType = ToVkDescriptorType(w.Type)};
        if (w.Type == DescriptorType::UniformBuffer || w.Type == DescriptorType::StorageBuffer)
        {
            Buffers.push_back(vk::DescriptorBufferInfo{
                .buffer = Buf(w.Buffer), .offset = w.Offset, .range = w.Range == kWholeSize ? VK_WHOLE_SIZE : w.Range});
            write.pBufferInfo = &Buffers.back();
        }
        else
        {
            VulkanTexture* texture = Tex(w.Texture);
            const bool storage = w.Type == DescriptorType::StorageTexture;
            Images.push_back(vk::DescriptorImageInfo{.sampler = storage ? vk::Sampler{} : *texture->GetSampler(),
                                                     .imageView = *texture->GetImageView(),
                                                     .imageLayout = storage ? vk::ImageLayout::eGeneral
                                                                            : vk::ImageLayout::eShaderReadOnlyOptimal});
            write.pImageInfo = &Images.back();
        }
        Writes.push_back(write);
    }
}

std::vector<vk::DescriptorSetLayout> PipelineSetLayouts(vk::DescriptorSetLayout global,
                                                        std::span<const DescriptorSetLayoutHandle> extra)
{
    std::vector<vk::DescriptorSetLayout> layouts{global};
    for (const DescriptorSetLayoutHandle handle : extra)
        if (handle != nullptr)
            layouts.push_back(*reinterpret_cast<const VulkanCoreSetLayout*>(handle)->Layout);
    return layouts;
}

} // namespace UHE::RHI::VULKAN
