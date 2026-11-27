// The RHI thin core on the Vulkan device (issue #30, RHICore.h): limits,
// native handles, readback, imported textures and views, queries, descriptor
// sets beyond the bindless one, and command lists with explicit submission.

#include "uhepch.h"
#include <algorithm>
#include "Platform/Vulkan/VulkanBuffer.h"
#include "Platform/Vulkan/VulkanCoreObjects.h"
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanExtensionCheck.h"
#include "Platform/Vulkan/VulkanTexture.h"

namespace UHE::RHI::VULKAN
{
namespace
{
u32 LargestSampleCount(vk::SampleCountFlags flags)
{
    for (const u32 count : {64u, 32u, 16u, 8u, 4u, 2u})
        if (flags & static_cast<vk::SampleCountFlagBits>(count))
            return count;
    return 1;
}
} // namespace

const DeviceLimits& VulkanDevice::GetLimits() const
{
    std::call_once(m_LimitsOnce,
                   [this]
                   {
                       DeviceLimits& l = m_Limits;
                       const vk::raii::PhysicalDevice& gpu = *m_Context.physicalDeviceHandle;
                       const vk::PhysicalDeviceProperties props = gpu.getProperties();
                       const vk::PhysicalDeviceFeatures features = gpu.getFeatures();
                       const vk::PhysicalDeviceLimits& limits = props.limits;
                       l.DeviceName = props.deviceName.data();
                       l.VendorID = props.vendorID;
                       l.DeviceID = props.deviceID;
                       l.ApiVersion = props.apiVersion;
                       l.MaxTexture2DSize = limits.maxImageDimension2D;
                       l.MaxTextureArrayLayers = limits.maxImageArrayLayers;
                       l.MaxColorAttachments = limits.maxColorAttachments;
                       l.MaxPushConstantsSize = limits.maxPushConstantsSize;
                       l.MaxBoundDescriptorSets = limits.maxBoundDescriptorSets;
                       l.MaxSamples = LargestSampleCount(limits.framebufferColorSampleCounts &
                                                         limits.framebufferDepthSampleCounts);
                       for (int i = 0; i < 3; ++i)
                       {
                           l.MaxComputeWorkGroupCount[i] = limits.maxComputeWorkGroupCount[i];
                           l.MaxComputeWorkGroupSize[i] = limits.maxComputeWorkGroupSize[i];
                       }
                       l.MaxComputeWorkGroupInvocations = limits.maxComputeWorkGroupInvocations;
                       l.MaxStorageBufferRange = limits.maxStorageBufferRange;
                       l.MinUniformBufferOffsetAlignment = limits.minUniformBufferOffsetAlignment;
                       l.MinStorageBufferOffsetAlignment = limits.minStorageBufferOffsetAlignment;
                       l.TimestampPeriodNs = limits.timestampPeriod;
                       l.Timestamps = limits.timestampComputeAndGraphics != VK_FALSE && limits.timestampPeriod > 0;
                       l.Wireframe = m_ExtensionCheck.SupportsWireframe();
                       l.TextureCompressionBC = features.textureCompressionBC != VK_FALSE;
                       l.TextureCompressionASTC = features.textureCompressionASTC_LDR != VK_FALSE;
                       l.TextureCompressionETC2 = features.textureCompressionETC2 != VK_FALSE;
                       l.TimelineSemaphores = m_ExtensionCheck.SupportsTimelineSemaphores();
                       l.Synchronization2 = m_ExtensionCheck.SupportsSync2();
                       l.DynamicRendering = m_ExtensionCheck.SupportsDynamicRendering();
                       l.PushDescriptors = m_ExtensionCheck.Supports(Extension::PushDescriptor);
                       auto& logical = const_cast<VulkanLogicalDevice&>(m_LogicalDevice);
                       l.DedicatedComputeQueue = logical.HasDedicatedCompute();
                       l.DedicatedTransferQueue = logical.HasDedicatedTransfer();
                   });
    return m_Limits;
}

void* VulkanDevice::GetNativeHandle(NativeObject object) const
{
    auto& self = const_cast<VulkanDevice&>(*this);
    switch (object)
    {
        case NativeObject::Instance:
            return static_cast<VkInstance>(**m_Context.instanceHandle);
        case NativeObject::PhysicalDevice:
            return static_cast<VkPhysicalDevice>(**m_Context.physicalDeviceHandle);
        case NativeObject::Device:
            return static_cast<VkDevice>(**m_Context.logicalDeviceHandle);
        case NativeObject::GraphicsQueue:
            return static_cast<VkQueue>(*self.m_LogicalDevice.getGraphicsQueue());
        case NativeObject::ComputeQueue:
            return static_cast<VkQueue>(*self.m_LogicalDevice.getComputeQueue());
        case NativeObject::TransferQueue:
            return static_cast<VkQueue>(*self.m_LogicalDevice.getTransferQueue());
        case NativeObject::Allocator:
            return m_Allocator;
    }
    return nullptr;
}

void* VulkanDevice::GetNativeTextureHandle(TextureHandle texture, NativeTextureObject object) const
{
    auto* vulkanTexture = reinterpret_cast<VulkanTexture*>(texture);
    if (vulkanTexture == nullptr)
        return nullptr;
    switch (object)
    {
        case NativeTextureObject::Image:
            return static_cast<VkImage>(vulkanTexture->GetImage());
        case NativeTextureObject::View:
            return static_cast<VkImageView>(*vulkanTexture->GetImageView());
        case NativeTextureObject::Sampler:
            return static_cast<VkSampler>(*vulkanTexture->GetSampler());
    }
    return nullptr;
}

void* VulkanDevice::GetNativeBufferHandle(BufferHandle buffer) const
{
    auto* vulkanBuffer = reinterpret_cast<VulkanBuffer*>(buffer);
    return vulkanBuffer != nullptr ? static_cast<VkBuffer>(vulkanBuffer->GetHandle()) : nullptr;
}

bool VulkanDevice::ReadBuffer(BufferHandle buffer, u64 offset, std::span<u8> out)
{
    auto* vulkanBuffer = reinterpret_cast<VulkanBuffer*>(buffer);
    return vulkanBuffer != nullptr && vulkanBuffer->ReadData(out.data(), out.size(), offset);
}

TextureHandle VulkanDevice::ImportTexture(const TextureImportDesc& desc)
{
    if (desc.NativeImage == nullptr)
        return nullptr;
    auto* texture = new VulkanTexture();
    texture->InitImported(*this, vk::Image(static_cast<VkImage>(desc.NativeImage)),
                          vk::ImageView(static_cast<VkImageView>(desc.NativeView)), desc.Desc);
    return reinterpret_cast<TextureHandle>(texture);
}

TextureHandle VulkanDevice::CreateTextureView(TextureHandle texture, const TextureViewDesc& desc)
{
    auto* parent = reinterpret_cast<VulkanTexture*>(texture);
    if (parent == nullptr)
        return nullptr;
    auto* view = new VulkanTexture();
    view->InitView(*this, *parent, desc);
    return reinterpret_cast<TextureHandle>(view);
}

// ── Queries ─────────────────────────────────────────────────────────────────

QueryPoolHandle VulkanDevice::CreateQueryPool(const QueryPoolDesc& desc)
{
    if (desc.Count == 0)
        return nullptr;
    auto* pool = new VulkanCoreQueryPool();
    pool->Type = desc.Type;
    pool->Count = desc.Count;
    pool->Pool = vk::raii::QueryPool(m_LogicalDevice.getLogicalDevice(),
                                     vk::QueryPoolCreateInfo{.queryType = desc.Type == QueryType::Timestamp
                                                                              ? vk::QueryType::eTimestamp
                                                                              : vk::QueryType::eOcclusion,
                                                             .queryCount = desc.Count});
    return reinterpret_cast<QueryPoolHandle>(pool);
}

void VulkanDevice::DestroyQueryPool(QueryPoolHandle pool)
{
    auto* object = reinterpret_cast<VulkanCoreQueryPool*>(pool);
    if (object != nullptr)
        DeferDestruction([object] { delete object; });
}

bool VulkanDevice::GetQueryResults(QueryPoolHandle pool, u32 first, std::span<u64> out, bool wait)
{
    auto* object = reinterpret_cast<VulkanCoreQueryPool*>(pool);
    if (object == nullptr || out.empty() || first + out.size() > object->Count)
        return false;
    vk::QueryResultFlags flags = vk::QueryResultFlagBits::e64;
    if (wait)
        flags |= vk::QueryResultFlagBits::eWait;
    const vk::Result result =
        (*m_LogicalDevice.getLogicalDevice())
            .getQueryPoolResults(*object->Pool, first, static_cast<u32>(out.size()), out.size_bytes(), out.data(),
                                 sizeof(u64), flags, *m_LogicalDevice.getLogicalDevice().getDispatcher());
    return result == vk::Result::eSuccess;
}

// ── Descriptor sets beyond the bindless one ─────────────────────────────────

DescriptorSetLayoutHandle VulkanDevice::CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc)
{
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    bindings.reserve(desc.Bindings.size());
    for (const DescriptorBinding& b : desc.Bindings)
        bindings.push_back(vk::DescriptorSetLayoutBinding{.binding = b.Binding,
                                                          .descriptorType = ToVkDescriptorType(b.Type),
                                                          .descriptorCount = b.Count,
                                                          .stageFlags = ToVkShaderStages(b.Stages)});
    vk::DescriptorSetLayoutCreateFlags flags{};
    if (desc.PushDescriptor)
    {
        UHE_CORE_ASSERT(GetLimits().PushDescriptors, "a PushDescriptor layout needs VK_KHR_push_descriptor");
        flags |= vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
    }
    auto* layout = new VulkanCoreSetLayout();
    layout->Desc = desc;
    layout->Layout = vk::raii::DescriptorSetLayout(
        m_LogicalDevice.getLogicalDevice(),
        vk::DescriptorSetLayoutCreateInfo{
            .flags = flags, .bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()});
    return reinterpret_cast<DescriptorSetLayoutHandle>(layout);
}

void VulkanDevice::DestroyDescriptorSetLayout(DescriptorSetLayoutHandle layout)
{
    auto* object = reinterpret_cast<VulkanCoreSetLayout*>(layout);
    if (object != nullptr)
        DeferDestruction([object] { delete object; });
}

DescriptorSetHandle VulkanDevice::CreateDescriptorSet(DescriptorSetLayoutHandle layout)
{
    const auto* setLayout = reinterpret_cast<const VulkanCoreSetLayout*>(layout);
    if (setLayout == nullptr || setLayout->Desc.PushDescriptor)
        return nullptr; // a push layout is written with PushDescriptors, never allocated
    std::vector<vk::DescriptorPoolSize> sizes;
    for (const DescriptorBinding& b : setLayout->Desc.Bindings)
        sizes.push_back(vk::DescriptorPoolSize{.type = ToVkDescriptorType(b.Type), .descriptorCount = b.Count});
    auto* set = new VulkanCoreDescriptorSet();
    set->Layout = setLayout;
    set->Pool = vk::raii::DescriptorPool(
        m_LogicalDevice.getLogicalDevice(),
        vk::DescriptorPoolCreateInfo{
            .flags = {}, .maxSets = 1, .poolSizeCount = static_cast<u32>(sizes.size()), .pPoolSizes = sizes.data()});
    const vk::DescriptorSetLayout handle = *setLayout->Layout;
    const vk::DescriptorSetAllocateInfo allocate{
        .descriptorPool = *set->Pool, .descriptorSetCount = 1, .pSetLayouts = &handle};
    set->Set = (*m_LogicalDevice.getLogicalDevice())
                   .allocateDescriptorSets(allocate, *m_LogicalDevice.getLogicalDevice().getDispatcher())
                   .front();
    return reinterpret_cast<DescriptorSetHandle>(set);
}

void VulkanDevice::UpdateDescriptorSet(DescriptorSetHandle set, std::span<const DescriptorWrite> writes)
{
    auto* object = reinterpret_cast<VulkanCoreDescriptorSet*>(set);
    if (object == nullptr || writes.empty())
        return;
    const VulkanDescriptorWrites vkWrites(object->Set, writes);
    m_LogicalDevice.getLogicalDevice().updateDescriptorSets(vkWrites.Writes, nullptr);
}

void VulkanDevice::DestroyDescriptorSet(DescriptorSetHandle set)
{
    auto* object = reinterpret_cast<VulkanCoreDescriptorSet*>(set);
    if (object != nullptr)
        DeferDestruction([object] { delete object; }); // the pool frees the set with it
}

// ── Command lists ───────────────────────────────────────────────────────────

RHICommandBuffer* VulkanDevice::BeginCommandList(const CommandListDesc& desc)
{
    return m_CommandLists.Begin(desc, m_CurrentFrame);
}

void VulkanDevice::Submit(const SubmitDesc& desc)
{
    m_CommandLists.Submit(desc, m_CurrentFrame);
}

} // namespace UHE::RHI::VULKAN
