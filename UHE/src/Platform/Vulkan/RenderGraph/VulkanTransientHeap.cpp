#include "uhepch.h"
#include "Platform/Vulkan/RenderGraph/VulkanTransientHeap.h"
#include <algorithm>
#include "Platform/Vulkan/VulkanContext.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanTexture.h"
#include "Platform/Vulkan/VulkanTypes.h"
#include "UHE/RHI/FrameGraph/TransientPlacement.h"

namespace UHE::RHI::VULKAN
{
namespace
{
/// The image VulkanTexture::Init would make for \p desc: same format, usage, mips, layers and samples.
vk::ImageCreateInfo ImageInfo(const TextureDesc& desc)
{
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
    return vk::ImageCreateInfo{.flags = {},
                               .imageType = vk::ImageType::e2D,
                               .format = MapTextureFormat(desc.format),
                               .extent = {desc.width, desc.height, 1},
                               .mipLevels = std::max(desc.mipLevels, 1u),
                               .arrayLayers = std::max(desc.arrayLayers, 1u),
                               .samples = ToVkSample(std::max(desc.sampleCount, 1u)),
                               .tiling = vk::ImageTiling::eOptimal,
                               .usage = usage,
                               .sharingMode = vk::SharingMode::eExclusive,
                               .initialLayout = vk::ImageLayout::eUndefined};
}

bool SameDesc(const TextureDesc& a, const TextureDesc& b)
{
    return a.width == b.width && a.height == b.height && a.depth == b.depth && a.mipLevels == b.mipLevels &&
           a.format == b.format && a.usage == b.usage && a.arrayLayers == b.arrayLayers &&
           a.sampleCount == b.sampleCount;
}
} // namespace

bool VulkanTransientHeap::SameShape(std::span<const Request> requests) const
{
    return requests.size() == m_Shape.size() &&
           std::equal(requests.begin(), requests.end(), m_Shape.begin(), [](const Request& a, const Request& b)
                      { return a.First == b.First && a.Last == b.Last && SameDesc(a.Desc, b.Desc); });
}

const std::vector<TextureHandle>& VulkanTransientHeap::Acquire(std::span<const Request> requests)
{
    if (!SameShape(requests))
        Build(requests);
    return m_Handles;
}

void VulkanTransientHeap::Build(std::span<const Request> requests)
{
    Clear();
    ++m_Stats.Rebuilds;
    m_Shape.assign(requests.begin(), requests.end());
    VulkanContext& context = m_Device.GetVulkanContext();
    const vk::raii::Device& device = *context.logicalDeviceHandle;

    std::vector<vk::ImageCreateInfo> infos;
    std::vector<PlacementRequest> placementRequests;
    infos.reserve(requests.size());
    placementRequests.reserve(requests.size());
    for (const Request& request : requests)
    {
        infos.push_back(ImageInfo(request.Desc));
        // What the image will need, asked of the device without making one (Vulkan 1.3).
        const vk::MemoryRequirements2 needs =
            device.getImageMemoryRequirements(vk::DeviceImageMemoryRequirements{.pCreateInfo = &infos.back()});
        placementRequests.push_back(PlacementRequest{.Size = needs.memoryRequirements.size,
                                                     .Alignment = needs.memoryRequirements.alignment,
                                                     .MemoryTypeBits = needs.memoryRequirements.memoryTypeBits,
                                                     .First = request.First,
                                                     .Last = request.Last});
    }
    const Placement placement = PlanTransientPlacement(placementRequests);

    m_Blocks.assign(placement.Blocks.size(), nullptr);
    for (std::size_t b = 0; b < placement.Blocks.size(); ++b)
    {
        const PlacementBlock& block = placement.Blocks[b];
        const VkMemoryRequirements needs{
            .size = block.Size, .alignment = block.Alignment, .memoryTypeBits = block.MemoryTypeBits};
        VmaAllocationCreateInfo create{};
        create.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (vmaAllocateMemory(context.allocator, &needs, &create, &m_Blocks[b], nullptr) != VK_SUCCESS)
            m_Blocks[b] = nullptr;
    }

    m_Images.assign(requests.size(), VK_NULL_HANDLE);
    m_Objects.assign(requests.size(), nullptr);
    m_Handles.assign(requests.size(), nullptr);
    for (std::size_t i = 0; i < requests.size(); ++i)
    {
        const VmaAllocation block = m_Blocks[placement.BlockOf[i]];
        if (block == nullptr)
            continue;
        const VkImageCreateInfo info = infos[i];
        if (vmaCreateAliasingImage2(context.allocator, block, 0, &info, &m_Images[i]) != VK_SUCCESS)
        {
            m_Images[i] = VK_NULL_HANDLE;
            continue;
        }
        // The texture wraps the image (view, sampler, bindless slot) and never frees it.
        auto* texture = new VulkanTexture();
        texture->InitImported(m_Device, vk::Image(m_Images[i]), nullptr, requests[i].Desc);
        m_Objects[i] = texture;
        m_Handles[i] = reinterpret_cast<TextureHandle>(texture);
    }

    m_Stats.RequestedBytes = placement.RequestedBytes;
    m_Stats.AllocatedBytes = placement.AllocatedBytes;
    m_Stats.Textures = static_cast<u32>(requests.size());
    m_Stats.Blocks = static_cast<u32>(placement.Blocks.size());
    UHE_CORE_INFO("[RENDERGRAPH] aliasing: {0} transient textures in {1} blocks, {2:.1f} MB instead of {3:.1f} MB",
                  m_Stats.Textures, m_Stats.Blocks, static_cast<double>(m_Stats.AllocatedBytes) / (1024.0 * 1024.0),
                  static_cast<double>(m_Stats.RequestedBytes) / (1024.0 * 1024.0));
}

void VulkanTransientHeap::Clear()
{
    if (m_Blocks.empty() && m_Objects.empty())
        return;
    // The textures first (their views and bindless slots, deferred by
    // VulkanTexture), then the images and the memory under them, deferred to
    // the same frame: a frame in flight may still read them.
    for (VulkanTexture* texture : m_Objects)
        delete texture;
    VmaAllocator allocator = m_Device.GetVulkanContext().allocator;
    VkDevice device = static_cast<VkDevice>(**m_Device.GetVulkanContext().logicalDeviceHandle);
    m_Device.DeferDestruction(
        [allocator, device, images = std::move(m_Images), blocks = std::move(m_Blocks)]
        {
            for (VkImage image : images)
                if (image != VK_NULL_HANDLE)
                    vk::Device(device).destroyImage(vk::Image(image), nullptr, VULKAN_HPP_DEFAULT_DISPATCHER);
            for (VmaAllocation block : blocks)
                if (block != nullptr)
                    vmaFreeMemory(allocator, block);
        });
    m_Images.clear();
    m_Blocks.clear();
    m_Objects.clear();
    m_Handles.clear();
    m_Shape.clear();
}

} // namespace UHE::RHI::VULKAN
