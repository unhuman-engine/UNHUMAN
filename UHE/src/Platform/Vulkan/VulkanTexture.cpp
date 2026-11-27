#include "uhepch.h"
#include "VulkanTexture.h"
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_raii.hpp>
#include "VulkanCommandPool.h"
#include "VulkanContext.h"
#include "VulkanDevice.h"
#include "VulkanLogicalDevice.h"
#include "VulkanTypes.h"
#include "VulkanUtils.h"

namespace UHE::RHI::VULKAN
{

void VulkanTexture::Init(VulkanDevice& device, const TextureDesc& desc)
{
    auto& ctx = GetVulkanContext();

    m_Device = &device;
    m_Desc = desc;
    m_Width = desc.width;
    m_Height = desc.height;
    m_MipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;
    m_allocator = ctx.allocator;

    vk::Format format = MapTextureFormat(desc.format);

    vk::ImageUsageFlags usage{};
    if (desc.usage & TextureUsage::Sampled)
        usage |= vk::ImageUsageFlagBits::eSampled;
    if (desc.usage & TextureUsage::ColorAttach)
        usage |= vk::ImageUsageFlagBits::eColorAttachment;
    if (desc.usage & TextureUsage::DepthAttach)
        usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
    if (desc.usage & TextureUsage::Storage)
        usage |= vk::ImageUsageFlagBits::eStorage;
    if (desc.usage & TextureUsage::TransferSrc)
        usage |= vk::ImageUsageFlagBits::eTransferSrc;
    if (desc.usage & TextureUsage::TransferDst)
        usage |= vk::ImageUsageFlagBits::eTransferDst;

    vk::ImageAspectFlags aspect = FormatToAspect(format);
    m_Aspect = aspect;
    m_LayerCount = std::max(desc.arrayLayers, 1u);

    CreatedImage created =
        ::UHE::RHI::VULKAN::CreateImage(m_Width, m_Height, format, usage, m_MipLevels, VMA_MEMORY_USAGE_GPU_ONLY,
                                        m_LayerCount, std::max(desc.sampleCount, 1u));
    textureImage = created.image;
    textureImageMemory = created.allocation;

    // A view needs a usage a view serves (VUID-VkImageViewCreateInfo-image-04441):
    // a texture that is only ever copied to and from has none.
    const bool viewable = (desc.usage & TextureUsage::Sampled) || (desc.usage & TextureUsage::ColorAttach) ||
                          (desc.usage & TextureUsage::DepthAttach) || (desc.usage & TextureUsage::Storage);
    if (viewable)
        textureImageView = m_LayerCount > 1
                               ? CreateImageView(textureImage, format, aspect, 0, m_MipLevels, 0, m_LayerCount, true)
                               : CreateImageView(textureImage, format, aspect, m_MipLevels);

    if (desc.usage & TextureUsage::Sampled)
    {
        // Clamped: these are render targets read back at screen UVs (post
        // effects, shadow maps, depth), where repeat would filter the opposite
        // edge into the border.
        textureSampler = CreateSampler(vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
                                       vk::SamplerAddressMode::eClampToEdge, static_cast<float>(m_MipLevels));
        vk::ImageView sampledView = *textureImageView;
        if (aspect & vk::ImageAspectFlagBits::eDepth)
        {
            // A sampled view names one aspect: a depth-stencil image is read
            // through a depth-only view, while attachments keep the full one.
            m_SampledDepthView = CreateImageView(textureImage, format, vk::ImageAspectFlagBits::eDepth, m_MipLevels);
            sampledView = *m_SampledDepthView;
        }
        m_TextureIndex =
            device.GetDescriptorManager()->BindTexture(*ctx.logicalDeviceHandle, sampledView, *textureSampler);
    }
}

vk::ImageSubresourceRange VulkanTexture::GetRange(u32 baseMip, u32 mipCount, u32 baseLayer, u32 layerCount) const
{
    const u32 mip = std::min(baseMip, m_MipLevels - 1);
    const u32 layer = std::min(baseLayer, m_LayerCount - 1);
    return vk::ImageSubresourceRange{.aspectMask = m_Aspect,
                                     .baseMipLevel = m_BaseMip + mip,
                                     .levelCount = std::min(mipCount, m_MipLevels - mip),
                                     .baseArrayLayer = m_BaseLayer + layer,
                                     .layerCount = std::min(layerCount, m_LayerCount - layer)};
}

void VulkanTexture::InitImported(VulkanDevice& device, vk::Image image, vk::ImageView view, const TextureDesc& desc)
{
    auto& ctx = GetVulkanContext();
    m_Device = &device;
    m_Desc = desc;
    m_Width = desc.width;
    m_Height = desc.height;
    m_MipLevels = std::max(desc.mipLevels, 1u);
    m_LayerCount = std::max(desc.arrayLayers, 1u);
    m_allocator = ctx.allocator;
    const vk::Format format = MapTextureFormat(desc.format);
    m_Aspect = FormatToAspect(format);
    // No allocation is recorded: the destructor then frees the views this
    // made, never the image, which belongs to whoever imported it.
    textureImage = image;
    textureImageMemory = nullptr;
    if (view)
        textureImageView = vk::raii::ImageView(*ctx.logicalDeviceHandle, view); // adopted: freed with the texture
    else
        textureImageView = m_LayerCount > 1
                               ? CreateImageView(image, format, m_Aspect, 0, m_MipLevels, 0, m_LayerCount, true)
                               : CreateImageView(image, format, m_Aspect, m_MipLevels);
    if (desc.usage & TextureUsage::Sampled)
    {
        textureSampler = CreateSampler(vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
                                       vk::SamplerAddressMode::eClampToEdge, static_cast<float>(m_MipLevels));
        m_TextureIndex =
            device.GetDescriptorManager()->BindTexture(*ctx.logicalDeviceHandle, *textureImageView, *textureSampler);
    }
}

void VulkanTexture::InitView(VulkanDevice& device, VulkanTexture& parent, const TextureViewDesc& desc)
{
    auto& ctx = GetVulkanContext();
    m_Device = &device;
    m_Desc = parent.m_Desc;
    if (desc.Format != TextureFormat::Undefined)
        m_Desc.format = desc.Format;
    m_allocator = ctx.allocator;
    m_Aspect = parent.m_Aspect;
    m_BaseMip = parent.m_BaseMip + std::min(desc.BaseMip, parent.m_MipLevels - 1);
    m_BaseLayer = parent.m_BaseLayer + std::min(desc.BaseLayer, parent.m_LayerCount - 1);
    m_MipLevels = std::min(desc.MipCount, parent.m_MipLevels - (m_BaseMip - parent.m_BaseMip));
    m_LayerCount = std::min(desc.LayerCount, parent.m_LayerCount - (m_BaseLayer - parent.m_BaseLayer));
    m_Width = std::max(parent.m_Width >> (m_BaseMip - parent.m_BaseMip), 1u);
    m_Height = std::max(parent.m_Height >> (m_BaseMip - parent.m_BaseMip), 1u);
    m_Desc.width = m_Width;
    m_Desc.height = m_Height;
    m_Desc.mipLevels = m_MipLevels;
    m_Desc.arrayLayers = m_LayerCount;
    textureImage = parent.textureImage; // not owned: no allocation recorded
    textureImageMemory = nullptr;
    const vk::Format format = MapTextureFormat(m_Desc.format);
    // A sampled view of depth names the depth aspect alone.
    const vk::ImageAspectFlags viewAspect =
        (m_Aspect & vk::ImageAspectFlagBits::eDepth) ? vk::ImageAspectFlagBits::eDepth : m_Aspect;
    textureImageView = CreateImageView(textureImage, format, viewAspect, m_BaseMip, m_MipLevels, m_BaseLayer,
                                       m_LayerCount, m_LayerCount > 1);
    if (m_Desc.usage & TextureUsage::Sampled)
    {
        textureSampler = CreateSampler(vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
                                       vk::SamplerAddressMode::eClampToEdge, static_cast<float>(m_MipLevels));
        m_TextureIndex =
            device.GetDescriptorManager()->BindTexture(*ctx.logicalDeviceHandle, *textureImageView, *textureSampler);
    }
}

VulkanTexture::~VulkanTexture()
{
    if (m_Device)
    {
        auto allocator = m_allocator;
        auto image = textureImage;
        auto imageMemory = textureImageMemory;

        // The views and sampler move into the deferred deletion, which runs
        // once no frame in flight can still read them.
        struct Owned
        {
            vk::raii::ImageView View;
            vk::raii::ImageView DepthView;
            vk::raii::Sampler Sampler;
        };
        auto owned = std::make_shared<Owned>(
            Owned{std::move(textureImageView), std::move(m_SampledDepthView), std::move(textureSampler)});
        auto ds = m_ImGuiDescriptorSet;
        auto slot = m_TextureIndex;
        auto* descriptorManager = m_Device->GetDescriptorManager();

        m_Device->DeferDestruction(
            [allocator, image, imageMemory, owned, ds, slot, descriptorManager]() mutable
            {
                if (ds != VK_NULL_HANDLE && ImGui::GetCurrentContext() != nullptr)
                {
                    ImGui_ImplVulkan_RemoveTexture(ds);
                }
                if (allocator && image && imageMemory)
                {
                    vmaDestroyImage(allocator, static_cast<VkImage>(image), imageMemory);
                }
                if (descriptorManager && slot != static_cast<u32>(-1) && slot != 0)
                {
                    descriptorManager->UnbindTexture(slot);
                }
                owned.reset();
            });

        textureImage = nullptr;
        textureImageMemory = nullptr;
        m_ImGuiDescriptorSet = VK_NULL_HANDLE;
    }
    else
    {
        if (m_ImGuiDescriptorSet != VK_NULL_HANDLE && ImGui::GetCurrentContext() != nullptr)
        {
            ImGui_ImplVulkan_RemoveTexture(m_ImGuiDescriptorSet);
            m_ImGuiDescriptorSet = VK_NULL_HANDLE;
        }

        m_SampledDepthView.clear();
        textureImageView.clear();
        textureSampler.clear();

        if (m_allocator && textureImage && textureImageMemory)
        {
            vmaDestroyImage(m_allocator, static_cast<VkImage>(textureImage), textureImageMemory);
            textureImage = nullptr;
            textureImageMemory = nullptr;
        }
    }
}

void* VulkanTexture::GetImGuiTextureID()
{
    if (m_ImGuiDescriptorSet == VK_NULL_HANDLE)
    {
        VkSampler sampler = *textureSampler;
        VkImageView imageView = *textureImageView;
        m_ImGuiDescriptorSet =
            ImGui_ImplVulkan_AddTexture(sampler, imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return (void*)m_ImGuiDescriptorSet;
}

void VulkanTexture::CreateImage(VulkanLogicalDevice& logDevice, uint32_t width, uint32_t height, uint32_t mipLevels,
                                vk::Format format, vk::ImageUsageFlags usage, VmaMemoryUsage memUsage,
                                vk::ImageTiling tiling, vk::Image& image, VmaAllocation& imageMemory)
{
    CreatedImage created = ::UHE::RHI::VULKAN::CreateImage(width, height, format, usage, mipLevels, memUsage);
    image = created.image;
    imageMemory = created.allocation;
}

// glTF filter enums map onto Vulkan's mag/min pair: a mip-selecting glTF filter
// only ever affects minification, so the magnification filter stays linear
// unless the file explicitly asked for nearest.
static vk::Filter ToVkFilter(SamplerDesc::Filter filter)
{
    switch (filter)
    {
        case SamplerDesc::Filter::Nearest:
        case SamplerDesc::Filter::NearestMipmapNearest:
        case SamplerDesc::Filter::NearestMipmapLinear:
            return vk::Filter::eNearest;
        case SamplerDesc::Filter::Linear:
        case SamplerDesc::Filter::LinearMipmapNearest:
        case SamplerDesc::Filter::LinearMipmapLinear:
        default:
            return vk::Filter::eLinear;
    }
}

static vk::SamplerAddressMode ToVkAddressMode(SamplerDesc::Wrap wrap)
{
    switch (wrap)
    {
        case SamplerDesc::Wrap::ClampToEdge:
            return vk::SamplerAddressMode::eClampToEdge;
        case SamplerDesc::Wrap::MirroredRepeat:
            return vk::SamplerAddressMode::eMirroredRepeat;
        case SamplerDesc::Wrap::Repeat:
        default:
            return vk::SamplerAddressMode::eRepeat;
    }
}

// sRGB is encoded in the IMAGE FORMAT, not the sampler: the hardware linearises
// on read only when the format carries the _Srgb suffix. A normal or
// metallicRoughness map created as _Srgb has its linear values gamma-decoded on
// every sample, which corrupts them.
static vk::Format ToVkTextureFormat(SamplerDesc::ColorSpace colorSpace)
{
    switch (colorSpace)
    {
        case SamplerDesc::ColorSpace::Linear:
            return vk::Format::eR8G8B8A8Unorm;
        case SamplerDesc::ColorSpace::HalfFloat:
            return vk::Format::eR16G16B16A16Sfloat;
        case SamplerDesc::ColorSpace::SRGB:
            break;
    }
    return vk::Format::eR8G8B8A8Srgb;
}

void VulkanTexture::CreateTexture(VulkanDevice& device, const void* pixelData, u32 width, u32 height, size_t dataSize)
{
    auto& ctx = GetVulkanContext();

    m_Device = &device;
    m_Width = width;
    m_Height = height;
    m_allocator = ctx.allocator;

    m_MipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc;

    const vk::Format format = ToVkTextureFormat(m_SamplerDesc.colorSpace);

    CreatedImage created = ::UHE::RHI::VULKAN::CreateImage(width, height, format, usage, m_MipLevels);
    textureImage = created.image;
    textureImageMemory = created.allocation;

    StagingBuffer staging = CreateStagingBuffer(dataSize);
    StagingBufferCopy(staging, pixelData, dataSize);

    ExecuteCopyCommand(device, staging.buffer, textureImage, width, height, m_MipLevels);
    DestroyStagingBuffer(staging);

    textureImageView = CreateImageView(textureImage, format, vk::ImageAspectFlagBits::eColor, m_MipLevels);

    // Sampler state comes from the loader when it supplied one (glTF declares
    // magFilter/minFilter/wrapS/wrapT per texture). Defaults reproduce exactly
    // what this call site hardcoded before, so untouched assets are unchanged.
    // maxLod is clamped to the chain we actually built; the desc's 1000.0
    // default means "whole chain" rather than a specific level count.
    textureSampler = CreateSampler(ToVkFilter(m_SamplerDesc.magFilter), ToVkFilter(m_SamplerDesc.minFilter),
                                   vk::SamplerMipmapMode::eLinear, ToVkAddressMode(m_SamplerDesc.wrapS),
                                   ToVkAddressMode(m_SamplerDesc.wrapT), ToVkAddressMode(m_SamplerDesc.wrapT),
                                   std::min(m_SamplerDesc.maxLod, static_cast<f32>(m_MipLevels)),
                                   static_cast<f32>(m_SamplerDesc.maxAnisotropy));

    m_TextureIndex =
        device.GetDescriptorManager()->BindTexture(*ctx.logicalDeviceHandle, *textureImageView, *textureSampler);
}

void VulkanTexture::CreateTextureWithMips(VulkanDevice& device, const void* pixelData, u32 width, u32 height,
                                          size_t dataSize, std::span<const UHE::MipLevel> mipChain)
{
    auto& ctx = GetVulkanContext();

    m_Device = &device;
    m_Width = width;
    m_Height = height;
    m_allocator = ctx.allocator;
    // The chain is authoritative: the image carries exactly its levels, and
    // the GPU generator below stays idle. \p mipChain is levels 1..N; level 0
    // is \p pixelData.
    UHE_CORE_ASSERT(mipChain.empty() || mipChain.front().width < width || mipChain.front().height < height,
                    "CreateTextureWithMips: the chain must start at level 1");
    m_MipLevels = 1 + static_cast<u32>(mipChain.size());

    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;

    const vk::Format format = ToVkTextureFormat(m_SamplerDesc.colorSpace);

    CreatedImage created = ::UHE::RHI::VULKAN::CreateImage(width, height, format, usage, m_MipLevels);
    textureImage = created.image;
    textureImageMemory = created.allocation;

    // One staging allocation for the whole chain, mip 0 first, then each
    // level's bytes at its running offset - the same layout ExecuteCopyCommand
    // walks when every level is precomputed.
    std::vector<u32> levelSizes;
    levelSizes.reserve(m_MipLevels);
    levelSizes.push_back(static_cast<u32>(dataSize));
    size_t totalSize = dataSize;
    for (const auto& mip : mipChain)
    {
        const size_t mipSize = RGBA8Size(mip.width, mip.height);
        levelSizes.push_back(static_cast<u32>(mipSize));
        totalSize += mipSize;
    }

    StagingBuffer staging = CreateStagingBuffer(totalSize);
    StagingBufferCopy(staging, pixelData, dataSize);
    size_t offset = dataSize;
    for (const auto& mip : mipChain)
    {
        const size_t mipSize = RGBA8Size(mip.width, mip.height);
        StagingBufferCopy(staging, mip.rgba.data(), mipSize, offset);
        offset += mipSize;
    }

    ExecuteCopyCommand(device, staging.buffer, textureImage, width, height, m_MipLevels, levelSizes);
    DestroyStagingBuffer(staging);

    textureImageView = CreateImageView(textureImage, format, vk::ImageAspectFlagBits::eColor, m_MipLevels);

    textureSampler = CreateSampler(ToVkFilter(m_SamplerDesc.magFilter), ToVkFilter(m_SamplerDesc.minFilter),
                                   vk::SamplerMipmapMode::eLinear, ToVkAddressMode(m_SamplerDesc.wrapS),
                                   ToVkAddressMode(m_SamplerDesc.wrapT), ToVkAddressMode(m_SamplerDesc.wrapT),
                                   std::min(m_SamplerDesc.maxLod, static_cast<f32>(m_MipLevels)),
                                   static_cast<f32>(m_SamplerDesc.maxAnisotropy));

    m_TextureIndex =
        device.GetDescriptorManager()->BindTexture(*ctx.logicalDeviceHandle, *textureImageView, *textureSampler);
}

void VulkanTexture::ExecuteCopyCommand(VulkanDevice& device, VkBuffer srcBuffer, vk::Image dstImage, uint32_t width,
                                       uint32_t height, uint32_t mipLevels, std::span<const u32> levelByteSizes)
{
    device.ImmediateSubmit(
        [&](vk::raii::CommandBuffer& cmd)
        {
            TransitionLayout(cmd, dstImage, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                             vk::AccessFlags{}, vk::AccessFlagBits::eTransferWrite,
                             vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
                             vk::ImageAspectFlagBits::eColor, mipLevels);

            // With level sizes supplied every level is copied straight out of the
            // staging buffer (a file-authored chain); without them only mip 0 is
            // copied - the staging buffer holds nothing else - and the GPU
            // generates the rest.
            u32 bufferOffset = 0;
            u32 levelWidth = width;
            u32 levelHeight = height;
            const u32 levelsToCopy = levelByteSizes.empty() ? 1 : mipLevels;
            for (u32 level = 0; level < levelsToCopy; ++level)
            {
                vk::BufferImageCopy region{.bufferOffset = bufferOffset,
                                           .bufferRowLength = 0,
                                           .bufferImageHeight = 0,
                                           .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                                                .mipLevel = level,
                                                                .baseArrayLayer = 0,
                                                                .layerCount = 1},
                                           .imageOffset = {0, 0, 0},
                                           .imageExtent = vk::Extent3D{levelWidth, levelHeight, 1}};

                cmd.copyBufferToImage(srcBuffer, dstImage, vk::ImageLayout::eTransferDstOptimal, region);

                bufferOffset += levelByteSizes.empty() ? static_cast<u32>(width) * height * 4 // unused beyond level 0
                                                       : levelByteSizes[level];
                levelWidth = std::max(levelWidth / 2, 1u);
                levelHeight = std::max(levelHeight / 2, 1u);
            }

            if (mipLevels == 1 || !levelByteSizes.empty())
            {
                // Nothing left to generate: every level is resident, so the whole
                // chain goes to shader read in one barrier.
                TransitionLayout(cmd, dstImage, vk::ImageLayout::eTransferDstOptimal,
                                 vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits::eTransferWrite,
                                 vk::AccessFlagBits::eShaderRead, vk::PipelineStageFlagBits::eTransfer,
                                 vk::PipelineStageFlagBits::eFragmentShader, vk::ImageAspectFlagBits::eColor,
                                 mipLevels);
            }
        });

    if (mipLevels > 1 && levelByteSizes.empty())
    {
        // The image was created with the colour-space-derived format
        // (ToVkTextureFormat in CreateTexture); the mip chain blits on the
        // same format.
        GenerateMipmaps(device, dstImage, ToVkTextureFormat(m_SamplerDesc.colorSpace), width, height, mipLevels);
    }
}

void VulkanTexture::GenerateMipmaps(VulkanDevice& device, vk::Image image, vk::Format imageFormat, int32_t texWidth,
                                    int32_t texHeight, uint32_t mipLevels)
{
    device.ImmediateSubmit(
        [&](vk::raii::CommandBuffer& cmd)
        {
            int32_t mipWidth = texWidth;
            int32_t mipHeight = texHeight;

            for (uint32_t i = 1; i < mipLevels; i++)
            {
                TransitionLayout(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                                 vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
                                 vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                                 vk::ImageAspectFlagBits::eColor, 1, i - 1);

                vk::ImageBlit blit{
                    .srcSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                       .mipLevel = i - 1,
                                       .baseArrayLayer = 0,
                                       .layerCount = 1},
                    .srcOffsets = {{vk::Offset3D{0, 0, 0}, vk::Offset3D{mipWidth, mipHeight, 1}}},
                    .dstSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                       .mipLevel = i,
                                       .baseArrayLayer = 0,
                                       .layerCount = 1},
                    .dstOffsets = {{vk::Offset3D{0, 0, 0}, vk::Offset3D{mipWidth > 1 ? mipWidth / 2 : 1,
                                                                        mipHeight > 1 ? mipHeight / 2 : 1, 1}}}};

                cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image, vk::ImageLayout::eTransferDstOptimal,
                              blit, vk::Filter::eNearest);

                TransitionLayout(cmd, image, vk::ImageLayout::eTransferSrcOptimal,
                                 vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits::eTransferRead,
                                 vk::AccessFlagBits::eShaderRead, vk::PipelineStageFlagBits::eTransfer,
                                 vk::PipelineStageFlagBits::eFragmentShader, vk::ImageAspectFlagBits::eColor, 1, i - 1);

                if (mipWidth > 1)
                    mipWidth /= 2;
                if (mipHeight > 1)
                    mipHeight /= 2;
            }

            TransitionLayout(cmd, image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                             vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                             vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
                             vk::ImageAspectFlagBits::eColor, 1, mipLevels - 1);
        });
}

void VulkanTexture::UpdateTexture(std::span<const u8> data)
{
    if (!m_Device)
        return;
    if (data.empty())
        return;

    m_allocator = GetVulkanContext().allocator;

    size_t size = data.size();
    StagingBuffer staging = CreateStagingBuffer(size);
    StagingBufferCopy(staging, data.data(), size);

    ExecuteCopyCommand(*m_Device, staging.buffer, textureImage, m_Width, m_Height, m_MipLevels);
    DestroyStagingBuffer(staging);
}

} // namespace UHE::RHI::VULKAN
