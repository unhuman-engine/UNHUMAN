#pragma once
/**
 * \file VulkanDescriptorSet.h
 * \brief Builder and owner of a single descriptor set and its layout.
 *
 * Descriptors are how shaders find buffers and images. This class lets callers add
 * bindings first (AddBinding), bake the layout (BuildLayout), then allocate and fill
 * the set (AllocateSet + Write*). Keeping the layout and the set together avoids the
 * usual "layout outlives set / set outlives pool" ordering bugs.
 *
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorSet.html
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/vkUpdateDescriptorSets.html
 */
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI::VULKAN
{
class VulkanDescriptorSet
{
public:
    VulkanDescriptorSet() = default;
    ~VulkanDescriptorSet() = default;
    VulkanDescriptorSet(const VulkanDescriptorSet&) = delete;
    VulkanDescriptorSet& operator=(const VulkanDescriptorSet&) = delete;

    // ── Builders ─────────────────────────────────────────────────────────────

    /// Adds a binding described by an RHI buffer-usage bitmask.
    void AddBinding(u32 binding, BufferUsageFlags usage, vk::ShaderStageFlags flags, u32 descriptorCount = 1);
    /// Adds a raw binding, optionally with extra binding flags (e.g. partially bound / update-after-bind).
    void AddBinding(u32 binding, vk::DescriptorType type, vk::ShaderStageFlags flags, u32 descriptorCount = 1,
                    vk::DescriptorBindingFlags bindingFlags = {});
    /// Bakes the accumulated bindings into a VkDescriptorSetLayout.
    void BuildLayout(vk::Device device);
    /// Allocates the set itself from \p pool (call after BuildLayout).
    void AllocateSet(vk::Device device, class VulkanDescriptorPool* pool);

    // ── Writers ──────────────────────────────────────────────────────────────

    void WriteBuffer(vk::Device device, u32 binding, vk::Buffer buffer, vk::DeviceSize size, vk::DeviceSize offset = 0);
    void WriteImage(vk::Device device, u32 binding, vk::ImageView imageView, vk::Sampler sampler,
                    vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal);

    void Bind();
    void DestroyDescriptorSet(vk::Device device);

    [[nodiscard]] vk::DescriptorSetLayout GetLayout() const { return m_DescriptorSetLayout; }
    [[nodiscard]] vk::DescriptorSet GetSet() const { return m_DescriptorSet; }

private:
    std::vector<vk::DescriptorSetLayoutBinding> m_BufferBinding;
    std::vector<vk::DescriptorBindingFlags> m_BindingFlags;
    vk::DescriptorSetLayout m_DescriptorSetLayout = nullptr;
    vk::DescriptorSet m_DescriptorSet = nullptr;
};
} // namespace UHE::RHI::VULKAN
