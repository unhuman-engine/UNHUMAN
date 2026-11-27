#pragma once
/**
 * \file VulkanTypes.h
 * \brief Translation layer between the neutral RHI vocabulary and Vulkan enums.
 *
 * Everything the engine expresses in RHITypes.h is lowered to a vk:: value here, so
 * the mappings live in exactly one place. The Stage/Access/ImageState enums are the
 * engine's own synchronisation vocabulary (UHE/RHI/RHIBarrier.h) — higher layers
 * reason in those, and only the barrier/submit encoders convert them to
 * VkPipelineStageFlags2 and friends.
 *
 * \see https://docs.vulkan.org/spec/latest/
 */
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHIBarrier.h"
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI::VULKAN
{

// ── Enum translation ─────────────────────────────────────────────────────────

/// Maps an RHI format to its vk::Format (depth formats round up to a supported layout).
[[nodiscard]] vk::Format MapTextureFormat(TextureFormat format);
[[nodiscard]] vk::PrimitiveTopology MapTopology(PrimitiveTopology topology);
[[nodiscard]] vk::Format ShaderDataTypeToVulkanFormat(ShaderDataType type);
[[nodiscard]] vk::Format ToVkFormat(TextureFormat format);
[[nodiscard]] vk::SampleCountFlagBits ToVkSample(u32 sampleCount);
[[nodiscard]] vk::AttachmentStoreOp ToVkStoreOp(StoreOp storeOp);
[[nodiscard]] vk::AttachmentLoadOp ToVkLoadOp(LoadOp loadOp);
[[nodiscard]] vk::ImageLayout ToVkImageLayout(TextureUsage usage);
/// \note Maps a BufferUsageFlags bitmask onto a VkDescriptorType — see that enum.
[[nodiscard]] vk::DescriptorType ToVkDescriptorType(BufferUsageFlags usage);

// ── Synchronisation vocabulary ───────────────────────────────────────────────
// Stage, Access and ImageState are the engine's own (UHE/RHI/RHIBarrier.h): the
// render graph reasons in them above the backend; this layer only translates.

// ── Sync-vocabulary translation ──────────────────────────────────────────────

[[nodiscard]] vk::ImageLayout ToVkImageLayout(ImageState state);
[[nodiscard]] vk::PipelineStageFlags2 ToVkPipelineStage2(Stage stage);
[[nodiscard]] vk::AccessFlags2 ToVkAccess2(Access access);
/// Lossy v1 fallbacks used only on the legacy (non-Sync2) tier.
[[nodiscard]] vk::PipelineStageFlags ToVkPipelineStage1(Stage stage);
[[nodiscard]] vk::AccessFlags ToVkAccess1(Access access);

} // namespace UHE::RHI::VULKAN
