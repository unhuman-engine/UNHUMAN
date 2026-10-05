#pragma once
/**
 * \file VulkanUtils.h
 * \brief Small, dependency-free Vulkan helpers shared across the backend.
 *
 * These wrap the repetitive parts of resource creation — staging uploads, image and
 * buffer allocation through VMA, layout transitions and one-shot submissions — so the
 * higher-level resource classes stay readable. They are free functions on purpose:
 * none of them own state beyond the context they borrow.
 *
 * \see https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/
 */
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI::VULKAN
{

/// Host-visible, device-local staging buffer used as a copy source for uploads.
struct StagingBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mappedData = nullptr;
    VkDeviceSize size = 0;
};

/// Allocates a persistently-mapped staging buffer of \p size bytes (transfer-src usage).
StagingBuffer CreateStagingBuffer(VkDeviceSize size);
/// Copies \p size bytes into the staging buffer's mapped memory and flushes the range.
void StagingBufferCopy(StagingBuffer& staging, const void* data, VkDeviceSize size);
// Writes at `offset` into the staging buffer, for multi-mip uploads that pack
// the whole chain into one allocation.
void StagingBufferCopy(StagingBuffer& staging, const void* data, VkDeviceSize size, VkDeviceSize offset);
/// Unmaps and frees a staging buffer. Safe to call on an already-destroyed buffer.
void DestroyStagingBuffer(StagingBuffer& staging);

// ── Image creation ───────────────────────────────────────────────────────────

struct CreatedImage
{
    vk::Image image = nullptr;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

/// Creates a 2D image; \p memUsage defaults to GPU-only (pair with a staging upload).
CreatedImage CreateImage(u32 width, u32 height, vk::Format format, vk::ImageUsageFlags usage, u32 mipLevels = 1,
                         VmaMemoryUsage memUsage = VMA_MEMORY_USAGE_GPU_ONLY);

/// Creates a full-range image view over \p image. \p aspect selects colour or depth/stencil.
[[nodiscard]] vk::raii::ImageView CreateImageView(vk::Image image, vk::Format format, vk::ImageAspectFlags aspect,
                                                  u32 mipLevels = 1);

// ── Buffer creation ──────────────────────────────────────────────────────────

struct CreatedBuffer
{
    vk::Buffer buffer = nullptr;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

CreatedBuffer CreateBuffer(VkDeviceSize size, vk::BufferUsageFlags usage, VmaMemoryUsage memUsage);

// ── Image layout transitions ─────────────────────────────────────────────────

/// Records a single image memory barrier. Convenience wrapper around vkCmdPipelineBarrier2.
void TransitionLayout(vk::raii::CommandBuffer& cmd, vk::Image image, vk::ImageLayout oldLayout,
                      vk::ImageLayout newLayout, vk::AccessFlags srcAccess, vk::AccessFlags dstAccess,
                      vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage,
                      vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor, u32 mipLevels = 1,
                      u32 baseMipLevel = 0, u32 layerCount = 1, u32 baseArrayLayer = 0);

// ── Sampler creation ─────────────────────────────────────────────────────────

// Per-axis variant, for glTF textures that declare wrapS and wrapT differently.
// maxAnisotropy is clamped to the device limit inside; 1.0 keeps anisotropy off
// (and is what every caller before the SamplerDesc plumbing used).
[[nodiscard]] vk::raii::Sampler CreateSampler(vk::Filter magFilter, vk::Filter minFilter,
                                              vk::SamplerMipmapMode mipmapMode, vk::SamplerAddressMode addressModeU,
                                              vk::SamplerAddressMode addressModeV, vk::SamplerAddressMode addressModeW,
                                              f32 maxLod, f32 maxAnisotropy = 1.0f);

[[nodiscard]] vk::raii::Sampler CreateSampler(vk::Filter magFilter = vk::Filter::eLinear,
                                              vk::Filter minFilter = vk::Filter::eLinear,
                                              vk::SamplerMipmapMode mipmapMode = vk::SamplerMipmapMode::eLinear,
                                              vk::SamplerAddressMode addressMode = vk::SamplerAddressMode::eRepeat,
                                              f32 maxLod = 1.0f);

// ── Format mapping ───────────────────────────────────────────────────────────

/// \returns the aspect mask (colour vs. depth ± stencil) implied by \p format.
[[nodiscard]] vk::ImageAspectFlags FormatToAspect(vk::Format format);
[[nodiscard]] bool FormatHasStencil(vk::Format format);

// ── Immediate submit ─────────────────────────────────────────────────────────

/// Records \p function into a one-shot command buffer, submits it and waits. Used by
/// upload paths (CreateTexture etc.) that cannot wait for the frame loop.
void ImmediateSubmit(std::function<void(vk::raii::CommandBuffer& cmd)>&& function);

} // namespace UHE::RHI::VULKAN
