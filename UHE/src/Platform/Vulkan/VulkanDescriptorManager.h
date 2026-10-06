#pragma once
/**
 * \file VulkanDescriptorManager.h
 * \brief Central descriptor-set owner for bindless buffers and textures.
 *
 * One global descriptor set holds every assignable buffer and texture slot, so a
 * shader can index them dynamically instead of rebinding per draw. Slots are handed
 * out on registration and recycled on unregistration; a freed slot's only cost is a
 * descriptor rewrite, never a layout change.
 *
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindDescriptorSets.html
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_indexing.html
 */
#include <deque>
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/VulkanDescriptorPool.h"
#include "Platform/Vulkan/VulkanDescriptorSet.h"

namespace UHE::RHI::VULKAN
{
/**
 * \brief Fluent collector for VkWriteDescriptorSet entries.
 *
 * Keeps its DescriptorBufferInfo/ImageInfo in deques for pointer stability, because
 * the write descriptors point into them until Build() runs.
 */
class DescriptorBuilder
{
public:
    DescriptorBuilder() = default;

    DescriptorBuilder& BindBuffer(u32 binding, vk::DescriptorBufferInfo* bufferInfo, vk::DescriptorType type,
                                  vk::ShaderStageFlags stageFlags);
    DescriptorBuilder& BindImage(u32 binding, vk::DescriptorImageInfo* imageInfo, vk::DescriptorType type,
                                 vk::ShaderStageFlags stageFlags);

    /// Writes one element of a bindless array (for indexed access from the shader).
    DescriptorBuilder& BindBufferArray(u32 binding, u32 arrayElement, vk::DescriptorBufferInfo* bufferInfo,
                                       vk::DescriptorType type);
    DescriptorBuilder& BindImageArray(u32 binding, u32 arrayElement, vk::DescriptorImageInfo* imageInfo,
                                      vk::DescriptorType type);

    void Build(vk::raii::Device& device, vk::DescriptorSet set);

private:
    std::vector<vk::WriteDescriptorSet> m_Writes;
    std::deque<vk::DescriptorBufferInfo> m_BufferInfos; // pointer-stable until Build()
    std::deque<vk::DescriptorImageInfo> m_ImageInfos;   // pointer-stable until Build()
};

class VulkanDevice;
class VulkanDescriptorManager
{
public:
    VulkanDescriptorManager() = default;
    VulkanDescriptorManager(const VulkanDescriptorManager&) = delete;
    VulkanDescriptorManager& operator=(const VulkanDescriptorManager&) = delete;

    void init(VulkanDevice& device);

    /// Registers \p buffer and \returns its bindless slot (recycled when possible).
    u32 RegisterBuffer(vk::raii::Device& device, vk::Buffer buffer, vk::DeviceSize size);
    /// Frees \p slot for reuse. The descriptor is not cleared; the next registrant overwrites it.
    void UnregisterBuffer(u32 slot);

    /// Binding 2 of the global set: the per-material data arrays the shader
    /// reads (MaterialGPU). A SEPARATE array from binding 0 because SPIR-V
    /// allows one descriptor type per binding - StructuredBuffer<LightData>
    /// and StructuredBuffer<MaterialGPU> cannot share binding 0 in the same
    /// module, which is exactly why the shader declares both.
    u32 RegisterMaterialBuffer(vk::raii::Device& device, vk::Buffer buffer, vk::DeviceSize size);
    /// Frees \p slot for reuse.
    void UnregisterMaterialBuffer(u32 slot);

    /// Registers \p imageView/\p sampler and \returns its bindless slot.
    u32 BindTexture(vk::raii::Device& device, vk::ImageView imageView, vk::Sampler sampler);
    /// Frees \p slot for reuse.
    void UnbindTexture(u32 slot);

    void UpdateDescriptorWithSameState(vk::raii::Device& device, vk::DescriptorSet DescriptorSet,
                                       DescriptorBuilder& builder);
    void UpdateDescriptorWithNewState(vk::raii::Device& device, vk::DescriptorSet DescriptorSet,
                                      DescriptorBuilder& builder);
    void cleanup();

    [[nodiscard]] vk::DescriptorSetLayout GetLayoutHandle() const { return m_GlobalDescriptorSet.GetLayout(); }
    [[nodiscard]] vk::DescriptorSet GetSetHandle() const { return m_GlobalDescriptorSet.GetSet(); }

    /// Mutable cursor accessors used while filling the bindless arrays.
    [[nodiscard]] u32& GetNextBufferIndex() { return m_NextBufferIndex; }
    [[nodiscard]] u32& GetNextMaterialBufferIndex() { return m_NextMaterialBufferIndex; }
    [[nodiscard]] u32& GetNextTextureIndex() { return m_NextTextureIndex; }

    [[nodiscard]] const u32& GetBufferIndex() const { return m_NextBufferIndex; }
    [[nodiscard]] const u32& GetTextureIndex() const { return m_NextTextureIndex; }
    [[nodiscard]] const u32& GetMaxBindlessResourceCount() const { return MAX_BINDLESS_RESOURCES; }

    [[nodiscard]] const vk::Device& GetDevice() const { return mdevice; }

    [[nodiscard]] const vk::raii::DescriptorPool& GetDescriptorPool() const
    {
        return m_FallbackDescriptorPool.GetDescriptorPool();
    }
    [[nodiscard]] vk::DescriptorSetLayout GetDescriptorSetLayout() const { return m_GlobalDescriptorSet.GetLayout(); }
    [[nodiscard]] vk::DescriptorSet GetDescriptorSet() const { return m_GlobalDescriptorSet.GetSet(); }

    [[nodiscard]] vk::raii::DescriptorPool& GetDescriptorPool() { return m_FallbackDescriptorPool.GetDescriptorPool(); }
    [[nodiscard]] vk::DescriptorSetLayout GetDescriptorSetLayout() { return m_GlobalDescriptorSet.GetLayout(); }
    [[nodiscard]] vk::DescriptorSet GetDescriptorSet() { return m_GlobalDescriptorSet.GetSet(); }

    [[nodiscard]] VulkanDescriptorPool* GetFallbackPool() { return &m_FallbackDescriptorPool; }

private:
    vk::Device mdevice;
    VulkanDescriptorSet m_GlobalDescriptorSet;
    VulkanDescriptorPool m_FallbackDescriptorPool;
    bool m_IsBindless = false;
    static constexpr uint32_t MAX_BINDLESS_RESOURCES = 10000;
    uint32_t m_NextBufferIndex = 0;
    uint32_t m_NextTextureIndex = 0;
    uint32_t m_NextMaterialBufferIndex = 0;
    std::vector<u32> m_FreeBufferIndices;
    std::vector<u32> m_FreeTextureIndices;
    std::vector<u32> m_FreeMaterialBufferIndices;
};
} // namespace UHE::RHI::VULKAN
