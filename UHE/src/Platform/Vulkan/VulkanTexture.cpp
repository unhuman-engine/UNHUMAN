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
    if (desc.usage & TextureUsage::Sampled)     usage |= vk::ImageUsageFlagBits::eSampled;
    if (desc.usage & TextureUsage::ColorAttach) usage |= vk::ImageUsageFlagBits::eColorAttachment;
    if (desc.usage & TextureUsage::DepthAttach) usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
    if (desc.usage & TextureUsage::Storage)     usage |= vk::ImageUsageFlagBits::eStorage;
    if (desc.usage & TextureUsage::TransferSrc) usage |= vk::ImageUsageFlagBits::eTransferSrc;
    if (desc.usage & TextureUsage::TransferDst) usage |= vk::ImageUsageFlagBits::eTransferDst;

    vk::ImageAspectFlags aspect = FormatToAspect(format);

    CreatedImage created = ::UHE::RHI::VULKAN::CreateImage(m_Width, m_Height, format, usage, m_MipLevels);
    textureImage = created.image;
    textureImageMemory = created.allocation;

    textureImageView = CreateImageView(textureImage, format, aspect, m_MipLevels);

    if (desc.usage & TextureUsage::Sampled)
    {
        textureSampler = CreateSampler( vk::Filter::eLinear, vk::Filter::eLinear,
                                        vk::SamplerMipmapMode::eLinear,
                                        vk::SamplerAddressMode::eRepeat,
                                        static_cast<float>(m_MipLevels));
        if (!(aspect & vk::ImageAspectFlagBits::eDepth))
        {
            m_TextureIndex = device.GetDescriptorManager()->BindTexture(*ctx.logicalDeviceHandle,
                                                                         *textureImageView, *textureSampler);
        }
    }
}

VulkanTexture::~VulkanTexture()
{
    if (m_Device)
    {
        auto allocator = m_allocator;
        auto image = textureImage;
        auto imageMemory = textureImageMemory;
        
        auto imageViewPtr = new vk::raii::ImageView(std::move(textureImageView));
        auto samplerPtr = new vk::raii::Sampler(std::move(textureSampler));
        auto ds = m_ImGuiDescriptorSet;
        auto slot = m_TextureIndex;
        auto* descriptorManager = m_Device->GetDescriptorManager();

        m_Device->DeferDestruction([allocator, image, imageMemory, imageViewPtr, samplerPtr, ds, slot, descriptorManager]() {
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
            delete imageViewPtr;
            delete samplerPtr;
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
        m_ImGuiDescriptorSet = ImGui_ImplVulkan_AddTexture(sampler, imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
    return colorSpace == SamplerDesc::ColorSpace::Linear ? vk::Format::eR8G8B8A8Unorm
                                                        : vk::Format::eR8G8B8A8Srgb;
}

void VulkanTexture::CreateTexture(VulkanDevice& device, const void* pixelData, u32 width, u32 height, size_t dataSize)
{
    auto& ctx = GetVulkanContext();

    m_Device = &device;
    m_Width = width;
    m_Height = height;
    m_allocator = ctx.allocator;

    m_MipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;

    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc;

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

    m_TextureIndex = device.GetDescriptorManager()->BindTexture(*ctx.logicalDeviceHandle,
                                                                 *textureImageView, *textureSampler);
}

void VulkanTexture::ExecuteCopyCommand(VulkanDevice& device, VkBuffer srcBuffer, vk::Image dstImage,
                                        uint32_t width, uint32_t height, uint32_t mipLevels)
{
    device.ImmediateSubmit([&](vk::raii::CommandBuffer& cmd)
    {
        TransitionLayout(cmd, dstImage,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::AccessFlags{}, vk::AccessFlagBits::eTransferWrite,
            vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
            vk::ImageAspectFlagBits::eColor, mipLevels);

        vk::BufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = vk::Extent3D{width, height, 1}
        };

        cmd.copyBufferToImage(srcBuffer, dstImage, vk::ImageLayout::eTransferDstOptimal, region);

        if (mipLevels == 1)
        {
            TransitionLayout(cmd, dstImage,
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader);
        }
    });

    if (mipLevels > 1)
    {
        // The image was created with the colour-space-derived format
        // (ToVkTextureFormat in CreateTexture); the mip chain blits on the
        // same format.
        GenerateMipmaps(device, dstImage, ToVkTextureFormat(m_SamplerDesc.colorSpace), width, height, mipLevels);
    }
}

void VulkanTexture::GenerateMipmaps(VulkanDevice& device, vk::Image image, vk::Format imageFormat,
                                    int32_t texWidth, int32_t texHeight, uint32_t mipLevels)
{
    device.ImmediateSubmit([&](vk::raii::CommandBuffer& cmd)
    {
        int32_t mipWidth = texWidth;
        int32_t mipHeight = texHeight;

        for (uint32_t i = 1; i < mipLevels; i++)
        {
            TransitionLayout(cmd, image,
                vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                vk::ImageAspectFlagBits::eColor, 1, i - 1);

            vk::ImageBlit blit{
                .srcSubresource = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = i - 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                },
                .srcOffsets = {{ vk::Offset3D{0, 0, 0}, vk::Offset3D{mipWidth, mipHeight, 1} }},
                .dstSubresource = {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = i,
                    .baseArrayLayer = 0,
                    .layerCount = 1
                },
                .dstOffsets = {{ vk::Offset3D{0, 0, 0}, vk::Offset3D{ mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 } }}
            };

            cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image, vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);

            TransitionLayout(cmd, image,
                vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
                vk::ImageAspectFlagBits::eColor, 1, i - 1);

            if (mipWidth > 1) mipWidth /= 2;
            if (mipHeight > 1) mipHeight /= 2;
        }

        TransitionLayout(cmd, image,
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
            vk::ImageAspectFlagBits::eColor, 1, mipLevels - 1);
    });
}

void VulkanTexture::UpdateTexture(std::span<const u8> data)
{
    if (!m_Device) return;
    if (data.empty()) return;

    m_allocator = GetVulkanContext().allocator;

    size_t size = data.size();
    StagingBuffer staging = CreateStagingBuffer(size);
    StagingBufferCopy(staging, data.data(), size);

    ExecuteCopyCommand(*m_Device, staging.buffer, textureImage, m_Width, m_Height, m_MipLevels);
    DestroyStagingBuffer(staging);
}

} // namespace UHE::RHI::VULKAN
