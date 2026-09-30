#pragma once
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI::VULKAN
{

// ─── Staging Buffer ──────────────────────────────────────────────
struct StagingBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mappedData = nullptr;
    VkDeviceSize size = 0;
};

StagingBuffer CreateStagingBuffer(VkDeviceSize size);
void StagingBufferCopy(StagingBuffer& staging, const void* data, VkDeviceSize size);
void DestroyStagingBuffer(StagingBuffer& staging);

// ─── Image Creation ──────────────────────────────────────────────
struct CreatedImage
{
    vk::Image image = nullptr;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

CreatedImage CreateImage(u32 width, u32 height, vk::Format format,
                         vk::ImageUsageFlags usage, u32 mipLevels = 1,
                         VmaMemoryUsage memUsage = VMA_MEMORY_USAGE_GPU_ONLY);

vk::raii::ImageView CreateImageView(vk::Image image, vk::Format format,
                                     vk::ImageAspectFlags aspect,
                                     u32 mipLevels = 1);

// ─── Buffer Creation ─────────────────────────────────────────────
struct CreatedBuffer
{
    vk::Buffer buffer = nullptr;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

CreatedBuffer CreateBuffer(VkDeviceSize size, vk::BufferUsageFlags usage,
                            VmaMemoryUsage memUsage);

// ─── Image Layout Transitions ────────────────────────────────────
void TransitionLayout(vk::raii::CommandBuffer& cmd, vk::Image image,
                       vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                       vk::AccessFlags srcAccess, vk::AccessFlags dstAccess,
                       vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage,
                       vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor,
                       u32 mipLevels = 1, u32 baseMipLevel = 0,
                       u32 layerCount = 1, u32 baseArrayLayer = 0);

// ─── Sampler Creation ────────────────────────────────────────────
vk::raii::Sampler CreateSampler(vk::Filter magFilter = vk::Filter::eLinear,
                                 vk::Filter minFilter = vk::Filter::eLinear,
                                 vk::SamplerMipmapMode mipmapMode = vk::SamplerMipmapMode::eLinear,
                                 vk::SamplerAddressMode addressMode = vk::SamplerAddressMode::eRepeat,
                                 f32 maxLod = 1.0f);

// Per-axis variant, for glTF textures that declare wrapS and wrapT differently.
vk::raii::Sampler CreateSampler(vk::Filter magFilter, vk::Filter minFilter, vk::SamplerMipmapMode mipmapMode,
                                 vk::SamplerAddressMode addressModeU, vk::SamplerAddressMode addressModeV,
                                 vk::SamplerAddressMode addressModeW, f32 maxLod);

// ─── Format Mapping ──────────────────────────────────────────────
vk::ImageAspectFlags FormatToAspect(vk::Format format);
bool FormatHasStencil(vk::Format format);

// ─── Immediate Submit ────────────────────────────────────────────
// Helper for one-shot upload commands (used by CreateTexture etc.)
void ImmediateSubmit(std::function<void(vk::raii::CommandBuffer& cmd)>&& function);

} // namespace UHE::RHI::VULKAN
