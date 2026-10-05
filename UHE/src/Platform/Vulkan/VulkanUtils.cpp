#include "uhepch.h"
#include "VulkanUtils.h"
#include "VulkanContext.h"
#include "VulkanDevice.h"

namespace UHE::RHI::VULKAN
{

VulkanContext* g_VulkanContext = nullptr;

// ─── Staging Buffer ──────────────────────────────────────────────

StagingBuffer CreateStagingBuffer(VkDeviceSize size)
{
    auto& ctx = GetVulkanContext();

    VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .pNext = nullptr,
                                  .flags = 0,
                                  .size = size,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                  .queueFamilyIndexCount = 0,
                                  .pQueueFamilyIndices = nullptr};

    VmaAllocationCreateInfo allocInfo{.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .usage = VMA_MEMORY_USAGE_AUTO,
                                      .requiredFlags = 0,
                                      .preferredFlags = 0,
                                      .memoryTypeBits = 0,
                                      .pool = VK_NULL_HANDLE,
                                      .pUserData = nullptr,
                                      .priority = 0.0f};

    StagingBuffer staging;
    staging.size = size;

    VkResult res =
        vmaCreateBuffer(ctx.allocator, &bufferInfo, &allocInfo, &staging.buffer, &staging.allocation, nullptr);
    if (res != VK_SUCCESS)
    {
        UHE_CORE_ERROR("Failed to create staging buffer!");
        return staging;
    }

    vmaMapMemory(ctx.allocator, staging.allocation, &staging.mappedData);
    return staging;
}

void StagingBufferCopy(StagingBuffer& staging, const void* data, VkDeviceSize size)
{
    if (staging.mappedData && data && size <= staging.size)
    {
        memcpy(staging.mappedData, data, size);
        vmaFlushAllocation(GetVulkanContext().allocator, staging.allocation, 0, size);
    }
}

void DestroyStagingBuffer(StagingBuffer& staging)
{
    if (!staging.buffer || !staging.allocation)
        return;

    auto& ctx = GetVulkanContext();
    if (staging.mappedData)
    {
        vmaUnmapMemory(ctx.allocator, staging.allocation);
        staging.mappedData = nullptr;
    }
    vmaDestroyBuffer(ctx.allocator, staging.buffer, staging.allocation);
    staging.buffer = VK_NULL_HANDLE;
    staging.allocation = VK_NULL_HANDLE;
    staging.size = 0;
}

// ─── Image Creation ──────────────────────────────────────────────

CreatedImage CreateImage(u32 width, u32 height, vk::Format format, vk::ImageUsageFlags usage, u32 mipLevels,
                         VmaMemoryUsage memUsage)
{
    auto& ctx = GetVulkanContext();

    vk::ImageCreateInfo imageInfo{.flags = {},
                                  .imageType = vk::ImageType::e2D,
                                  .format = format,
                                  .extent = {width, height, 1},
                                  .mipLevels = mipLevels,
                                  .arrayLayers = 1,
                                  .samples = vk::SampleCountFlagBits::e1,
                                  .tiling = vk::ImageTiling::eOptimal,
                                  .usage = usage,
                                  .sharingMode = vk::SharingMode::eExclusive,
                                  .queueFamilyIndexCount = 0,
                                  .pQueueFamilyIndices = nullptr,
                                  .initialLayout = vk::ImageLayout::eUndefined};

    VkImageCreateInfo rawImageInfo = static_cast<VkImageCreateInfo>(imageInfo);
    VmaAllocationCreateInfo allocInfo{.flags = 0,
                                      .usage = memUsage,
                                      .requiredFlags = 0,
                                      .preferredFlags = 0,
                                      .memoryTypeBits = 0,
                                      .pool = VK_NULL_HANDLE,
                                      .pUserData = nullptr,
                                      .priority = 0.0f};

    CreatedImage result;
    VkImage rawImage;
    VkResult res = vmaCreateImage(ctx.allocator, &rawImageInfo, &allocInfo, &rawImage, &result.allocation, nullptr);
    if (res != VK_SUCCESS)
    {
        UHE_CORE_ERROR("Failed to create image via VMA!");
        return result;
    }

    result.image = vk::Image(rawImage);
    return result;
}

vk::raii::ImageView CreateImageView(vk::Image image, vk::Format format, vk::ImageAspectFlags aspect, u32 mipLevels)
{
    auto& ctx = GetVulkanContext();

    vk::ImageViewCreateInfo viewInfo{
        .flags = {},
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = {},
        .subresourceRange = {
            .aspectMask = aspect, .baseMipLevel = 0, .levelCount = mipLevels, .baseArrayLayer = 0, .layerCount = 1}};

    return vk::raii::ImageView(*ctx.logicalDeviceHandle, viewInfo);
}

// ─── Buffer Creation ─────────────────────────────────────────────

CreatedBuffer CreateBuffer(VkDeviceSize size, vk::BufferUsageFlags usage, VmaMemoryUsage memUsage)
{
    auto& ctx = GetVulkanContext();

    VkBufferCreateInfo bufferInfo{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .pNext = nullptr,
                                  .flags = 0,
                                  .size = size,
                                  .usage = static_cast<VkBufferUsageFlags>(usage),
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                  .queueFamilyIndexCount = 0,
                                  .pQueueFamilyIndices = nullptr};

    VmaAllocationCreateInfo allocInfo{.flags = 0,
                                      .usage = memUsage,
                                      .requiredFlags = 0,
                                      .preferredFlags = 0,
                                      .memoryTypeBits = 0,
                                      .pool = VK_NULL_HANDLE,
                                      .pUserData = nullptr,
                                      .priority = 0.0f};

    if (memUsage == VMA_MEMORY_USAGE_CPU_ONLY || memUsage == VMA_MEMORY_USAGE_CPU_TO_GPU)
    {
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }

    CreatedBuffer result;
    VkBuffer rawBuffer;
    VkResult res = vmaCreateBuffer(ctx.allocator, &bufferInfo, &allocInfo, &rawBuffer, &result.allocation, nullptr);
    if (res != VK_SUCCESS)
    {
        UHE_CORE_ERROR("Failed to create buffer via VMA!");
        return result;
    }

    result.buffer = vk::Buffer(rawBuffer);
    return result;
}

// ─── Image Layout Transitions ────────────────────────────────────

void TransitionLayout(vk::raii::CommandBuffer& cmd, vk::Image image, vk::ImageLayout oldLayout,
                      vk::ImageLayout newLayout, vk::AccessFlags srcAccess, vk::AccessFlags dstAccess,
                      vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage, vk::ImageAspectFlags aspect,
                      u32 mipLevels, u32 baseMipLevel, u32 layerCount, u32 baseArrayLayer)
{
    vk::ImageMemoryBarrier barrier{.srcAccessMask = srcAccess,
                                   .dstAccessMask = dstAccess,
                                   .oldLayout = oldLayout,
                                   .newLayout = newLayout,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange = {.aspectMask = aspect,
                                                        .baseMipLevel = baseMipLevel,
                                                        .levelCount = mipLevels,
                                                        .baseArrayLayer = baseArrayLayer,
                                                        .layerCount = layerCount}};

    cmd.pipelineBarrier(srcStage, dstStage, {}, nullptr, nullptr, barrier);
}

// ─── Sampler Creation ────────────────────────────────────────────

vk::raii::Sampler CreateSampler(vk::Filter magFilter, vk::Filter minFilter, vk::SamplerMipmapMode mipmapMode,
                                vk::SamplerAddressMode addressMode, f32 maxLod)
{
    return CreateSampler(magFilter, minFilter, mipmapMode, addressMode, addressMode, addressMode, maxLod);
}

// Per-axis addressing overload. glTF declares wrapS and wrapT independently, so
// the single-addressMode form cannot express e.g. clamp-U / repeat-V, which is
// common on atlas and foliage textures. All three axes are taken separately
// because the existing signature applied one mode to all of them.
vk::raii::Sampler CreateSampler(vk::Filter magFilter, vk::Filter minFilter, vk::SamplerMipmapMode mipmapMode,
                                vk::SamplerAddressMode addressModeU, vk::SamplerAddressMode addressModeV,
                                vk::SamplerAddressMode addressModeW, f32 maxLod, f32 maxAnisotropy)
{
    auto& ctx = GetVulkanContext();

    // Anisotropy needs the feature enabled AND the value clamped to what the
    // device actually supports; requesting more than the limit is a hard
    // validation error, not a silent clamp.
    f32 deviceMax = ctx.physicalDeviceHandle->getProperties().limits.maxSamplerAnisotropy;
    f32 anisotropy = std::clamp(maxAnisotropy, 1.0f, deviceMax);

    vk::SamplerCreateInfo samplerInfo{.flags = {},
                                      .magFilter = magFilter,
                                      .minFilter = minFilter,
                                      .mipmapMode = mipmapMode,
                                      .addressModeU = addressModeU,
                                      .addressModeV = addressModeV,
                                      .addressModeW = addressModeW,
                                      .mipLodBias = 0.0f,
                                      .anisotropyEnable = anisotropy > 1.0f ? vk::Bool32{VK_TRUE} : vk::Bool32{VK_FALSE},
                                      .maxAnisotropy = anisotropy,
                                      .compareEnable = VK_FALSE,
                                      .compareOp = vk::CompareOp::eAlways,
                                      .minLod = 0.0f,
                                      .maxLod = maxLod,
                                      .borderColor = vk::BorderColor::eIntOpaqueBlack,
                                      .unnormalizedCoordinates = VK_FALSE};

    return vk::raii::Sampler(*ctx.logicalDeviceHandle, samplerInfo);
}

// ─── Format Mapping ──────────────────────────────────────────────

vk::ImageAspectFlags FormatToAspect(vk::Format format)
{
    switch (format)
    {
        case vk::Format::eD32SfloatS8Uint:
        case vk::Format::eD24UnormS8Uint:
            return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        case vk::Format::eD32Sfloat:
        case vk::Format::eD16Unorm:
            return vk::ImageAspectFlagBits::eDepth;
        default:
            return vk::ImageAspectFlagBits::eColor;
    }
}

bool FormatHasStencil(vk::Format format)
{
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint;
}

void ImmediateSubmit(std::function<void(vk::raii::CommandBuffer& cmd)>&& function)
{
    auto& ctx = GetVulkanContext();
    ctx.device->ImmediateSubmit(std::move(function));
}

} // namespace UHE::RHI::VULKAN
