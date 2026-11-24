#include "uhepch.h"
#include "VulkanDescriptorPool.h"
#include "VulkanDescriptorSet.h"
#include "VulkanDevice.h"
#include "vulkan/vulkan.hpp"

namespace UHE::RHI::VULKAN
{

DescriptorBuilder& DescriptorBuilder::BindBuffer(u32 binding, vk::DescriptorBufferInfo* bufferInfo,
                                                 vk::DescriptorType type, vk::ShaderStageFlags stageFlags)
{
    m_BufferInfos.push_back(*bufferInfo);
    vk::WriteDescriptorSet newWrite{.dstSet = nullptr,
                                    .dstBinding = binding,
                                    .dstArrayElement = 0,
                                    .descriptorCount = 1,
                                    .descriptorType = type,
                                    .pImageInfo = nullptr,
                                    .pBufferInfo = &m_BufferInfos.back(),
                                    .pTexelBufferView = nullptr};
    m_Writes.push_back(newWrite);
    return *this;
}

DescriptorBuilder& DescriptorBuilder::BindImage(u32 binding, vk::DescriptorImageInfo* imageInfo,
                                                vk::DescriptorType type, vk::ShaderStageFlags stageFlags)
{
    m_ImageInfos.push_back(*imageInfo);
    vk::WriteDescriptorSet newWrite{.dstSet = nullptr,
                                    .dstBinding = binding,
                                    .dstArrayElement = 0,
                                    .descriptorCount = 1,
                                    .descriptorType = type,
                                    .pImageInfo = &m_ImageInfos.back(),
                                    .pBufferInfo = nullptr,
                                    .pTexelBufferView = nullptr};
    m_Writes.push_back(newWrite);
    return *this;
}

DescriptorBuilder& DescriptorBuilder::BindBufferArray(u32 binding, u32 arrayElement,
                                                      vk::DescriptorBufferInfo* bufferInfo, vk::DescriptorType type)
{
    m_BufferInfos.push_back(*bufferInfo);
    vk::WriteDescriptorSet newWrite{.dstSet = nullptr,
                                    .dstBinding = binding,
                                    .dstArrayElement = arrayElement,
                                    .descriptorCount = 1,
                                    .descriptorType = type,
                                    .pImageInfo = nullptr,
                                    .pBufferInfo = &m_BufferInfos.back(),
                                    .pTexelBufferView = nullptr};
    m_Writes.push_back(newWrite);
    return *this;
}

DescriptorBuilder& DescriptorBuilder::BindImageArray(u32 binding, u32 arrayElement, vk::DescriptorImageInfo* imageInfo,
                                                     vk::DescriptorType type)
{
    m_ImageInfos.push_back(*imageInfo);
    vk::WriteDescriptorSet newWrite{.dstSet = nullptr,
                                    .dstBinding = binding,
                                    .dstArrayElement = arrayElement,
                                    .descriptorCount = 1,
                                    .descriptorType = type,
                                    .pImageInfo = &m_ImageInfos.back(),
                                    .pBufferInfo = nullptr,
                                    .pTexelBufferView = nullptr};
    m_Writes.push_back(newWrite);
    return *this;
}

void DescriptorBuilder::Build(vk::raii::Device& device, vk::DescriptorSet set)
{
    for (auto& write : m_Writes)
    {
        write.dstSet = set;
    }
    device.updateDescriptorSets(m_Writes, nullptr);
    m_Writes.clear();
    m_BufferInfos.clear();
    m_ImageInfos.clear();
}

void VulkanDescriptorManager::init(VulkanDevice& device)
{
    const auto& logicaldevice = device.getLogicalDevClass().getLogicalDevice();
    mdevice = *logicaldevice;
    m_IsBindless = device.GetVulkanContext().CheckExtensions->Supports(Extension::DescriptorIndexing);

    if (m_IsBindless)
    {
        m_FallbackDescriptorPool.Init(&device.GetVulkanContext());
        m_FallbackDescriptorPool.CreateBindlessDescriptorPool(MAX_BINDLESS_RESOURCES);

        vk::DescriptorBindingFlags flags =
            vk::DescriptorBindingFlagBits::eUpdateAfterBind | vk::DescriptorBindingFlagBits::ePartiallyBound;
        m_GlobalDescriptorSet.AddBinding(
            0, vk::DescriptorType::eStorageBuffer,
            vk::ShaderStageFlags(vk::ShaderStageFlagBits::eAllGraphics | vk::ShaderStageFlagBits::eCompute),
            MAX_BINDLESS_RESOURCES, flags);
        m_GlobalDescriptorSet.AddBinding(
            1, vk::DescriptorType::eCombinedImageSampler,
            vk::ShaderStageFlags(vk::ShaderStageFlagBits::eAllGraphics | vk::ShaderStageFlagBits::eCompute),
            MAX_BINDLESS_RESOURCES, flags);
        // Binding 2: per-material StructuredBuffer<MaterialGPU> arrays
        // (Basic3D.slang). Same descriptor type as binding 0 but a different
        // array - SPIR-V gives one type per binding, and LightData/BoneMatrix
        // and MaterialGPU are different structs.
        m_GlobalDescriptorSet.AddBinding(
            2, vk::DescriptorType::eStorageBuffer,
            vk::ShaderStageFlags(vk::ShaderStageFlagBits::eAllGraphics | vk::ShaderStageFlagBits::eCompute),
            MAX_BINDLESS_RESOURCES, flags);

        m_GlobalDescriptorSet.BuildLayout(mdevice);
        m_GlobalDescriptorSet.AllocateSet(mdevice, &m_FallbackDescriptorPool);
    }
    else
    {
        // Device selection already rejects such a GPU; this guards a context
        // built some other way. Handing out slots of a set that does not exist
        // made every shader index garbage.
        throw std::runtime_error("VulkanDescriptorManager: the device has no descriptor indexing, which the "
                                 "bindless global set requires");
    }
}

u32 VulkanDescriptorManager::AcquireSlot(std::vector<u32>& freeList, u32& next, const char* what)
{
    if (!freeList.empty())
    {
        const u32 slot = freeList.back();
        freeList.pop_back();
        return slot;
    }
    if (next < MAX_BINDLESS_RESOURCES)
    {
        return next++;
    }

    static bool reported = false;
    if (!reported)
    {
        reported = true;
        UHE_CORE_ERROR("Vulkan: all {} bindless {} slots are in use; new ones alias slot 0", MAX_BINDLESS_RESOURCES,
                       what);
    }
    return 0;
}

u32 VulkanDescriptorManager::RegisterBuffer(vk::raii::Device& device, vk::Buffer buffer, vk::DeviceSize size)
{
    if (!m_IsBindless)
        return kInvalidSlot;

    const u32 bindingIndex = AcquireSlot(m_FreeBufferIndices, m_NextBufferIndex, "buffer");

    vk::DescriptorBufferInfo bufferInfo{.buffer = buffer, .offset = 0, .range = size};

    DescriptorBuilder builder;
    builder.BindBufferArray(0, bindingIndex, &bufferInfo, vk::DescriptorType::eStorageBuffer)
        .Build(device, m_GlobalDescriptorSet.GetSet());

    return bindingIndex;
}

/// @note Slot 0 is never recycled: once the arrays are exhausted it is the
/// alias every extra registrant gets, so freeing it would hand it out twice.
void VulkanDescriptorManager::UnregisterBuffer(u32 slot)
{
    if (slot != kInvalidSlot && slot != 0)
    {
        m_FreeBufferIndices.push_back(slot);
    }
}

u32 VulkanDescriptorManager::RegisterMaterialBuffer(vk::raii::Device& device, vk::Buffer buffer, vk::DeviceSize size)
{
    if (!m_IsBindless)
        return kInvalidSlot;

    const u32 bindingIndex = AcquireSlot(m_FreeMaterialBufferIndices, m_NextMaterialBufferIndex, "material buffer");

    vk::DescriptorBufferInfo bufferInfo{.buffer = buffer, .offset = 0, .range = size};

    DescriptorBuilder builder;
    builder.BindBufferArray(2, bindingIndex, &bufferInfo, vk::DescriptorType::eStorageBuffer)
        .Build(device, m_GlobalDescriptorSet.GetSet());

    return bindingIndex;
}

void VulkanDescriptorManager::UnregisterMaterialBuffer(u32 slot)
{
    if (slot != kInvalidSlot && slot != 0)
    {
        m_FreeMaterialBufferIndices.push_back(slot);
    }
}
u32 VulkanDescriptorManager::BindTexture(vk::raii::Device& device, vk::ImageView imageView, vk::Sampler sampler)
{
    if (!m_IsBindless)
        return kInvalidSlot;

    const u32 slot = AcquireSlot(m_FreeTextureIndices, m_NextTextureIndex, "texture");

    vk::DescriptorImageInfo imageInfo{
        .sampler = sampler, .imageView = imageView, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};

    DescriptorBuilder builder;
    builder.BindImageArray(1, slot, &imageInfo, vk::DescriptorType::eCombinedImageSampler)
        .Build(device, m_GlobalDescriptorSet.GetSet());

    return slot;
}

void VulkanDescriptorManager::UnbindTexture(u32 slot)
{
    if (slot != kInvalidSlot && slot != 0)
    {
        m_FreeTextureIndices.push_back(slot);
    }
}

void VulkanDescriptorManager::UpdateDescriptorWithSameState(vk::raii::Device& device, vk::DescriptorSet DescriptorSet,
                                                            DescriptorBuilder& builder)
{
    builder.Build(device, DescriptorSet);
}

void VulkanDescriptorManager::UpdateDescriptorWithNewState(vk::raii::Device& device, vk::DescriptorSet DescriptorSet,
                                                           DescriptorBuilder& builder)
{
    builder.Build(device, DescriptorSet);
}

void VulkanDescriptorManager::cleanup()
{
    if (m_IsBindless)
    {
        m_GlobalDescriptorSet.DestroyDescriptorSet(mdevice);
        m_FallbackDescriptorPool.DestroyDescriptorPool();
        m_FreeBufferIndices.clear();
        m_FreeTextureIndices.clear();
        m_NextBufferIndex = 0;
        m_NextTextureIndex = 0;
    }
    else
    {
        m_FallbackDescriptorPool.DestroyDescriptorPool();
    }
}
} // namespace UHE::RHI::VULKAN
