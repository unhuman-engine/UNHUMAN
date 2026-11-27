#pragma once
/**
 * @file VulkanCoreObjects.h
 * @brief What the RHI core's opaque handles point at in the Vulkan backend (issue #30).
 *
 * QueryPoolHandle, DescriptorSetLayoutHandle and DescriptorSetHandle are
 * pointers to these; the device creates and destroys them (VulkanDeviceCore.cpp)
 * and the command buffer records with them (VulkanCommandBufferCore.cpp).
 */
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHICore.h"

namespace UHE::RHI::VULKAN
{

struct VulkanCoreQueryPool
{
    vk::raii::QueryPool Pool{nullptr};
    QueryType Type = QueryType::Timestamp;
    u32 Count = 0;
};

struct VulkanCoreSetLayout
{
    vk::raii::DescriptorSetLayout Layout{nullptr};
    DescriptorSetLayoutDesc Desc;
};

struct VulkanCoreDescriptorSet
{
    vk::raii::DescriptorPool Pool{nullptr}; ///< one small pool per set: sets are few and long-lived
    vk::DescriptorSet Set = nullptr;
    const VulkanCoreSetLayout* Layout = nullptr;
};

[[nodiscard]] vk::DescriptorType ToVkDescriptorType(DescriptorType type);
[[nodiscard]] vk::ShaderStageFlags ToVkShaderStages(ShaderStage stages);

/// The Vulkan structures for a list of writes; \p set may be null for push descriptors.
struct VulkanDescriptorWrites
{
    std::vector<vk::DescriptorBufferInfo> Buffers;
    std::vector<vk::DescriptorImageInfo> Images;
    std::vector<vk::WriteDescriptorSet> Writes;

    VulkanDescriptorWrites(vk::DescriptorSet set, std::span<const DescriptorWrite> writes);
};

/// The pipeline layout's set layouts: the bindless set 0, then the desc's sets 1..3.
[[nodiscard]] std::vector<vk::DescriptorSetLayout> PipelineSetLayouts(vk::DescriptorSetLayout global,
                                                                      std::span<const DescriptorSetLayoutHandle> extra);

} // namespace UHE::RHI::VULKAN
