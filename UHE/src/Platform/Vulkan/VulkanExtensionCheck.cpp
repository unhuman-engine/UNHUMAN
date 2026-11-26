#include "uhepch.h"
#include "VulkanExtensionCheck.h"
#include <vector>
#include <volk.h>
#include <vulkan/vulkan_core.h>
#include "vulkan/vulkan.hpp"

namespace UHE::RHI::VULKAN
{

const char *SyncTierName(SyncTier tier)
{
    switch (tier)
    {
    case SyncTier::Legacy:
        return "Legacy";
    case SyncTier::Sync2:
        return "Sync2";
    case SyncTier::Sync2Timeline:
        return "Sync2Timeline";
    }

    return "Unknown";
}

namespace
{
// The single place an extension name appears. CoreMajor/CoreMinor is the API
// version that promoted it (0, 0 means it was never promoted). Enable marks the
// extensions the engine asks for; every enabled extension that the device
// advertises ends up in the device-extension list.
struct ExtensionInfo
{
    Extension Id;
    const char *Name;
    uint32_t CoreMajor;
    uint32_t CoreMinor;
    bool Enable;
};

constexpr ExtensionInfo kExtensionInfo[] = {
    // Promoted to core.
    {Extension::ShaderDrawParameters, "VK_KHR_shader_draw_parameters", 1, 1, true},
    {Extension::Storage16Bit, "VK_KHR_16bit_storage", 1, 1, true},
    {Extension::Multiview, "VK_KHR_multiview", 1, 1, true},
    {Extension::SamplerYcbcrConversion, "VK_KHR_sampler_ycbcr_conversion", 1, 1, true},
    {Extension::Maintenance1, "VK_KHR_maintenance1", 1, 1, false},
    {Extension::Maintenance2, "VK_KHR_maintenance2", 1, 1, false},
    {Extension::Maintenance3, "VK_KHR_maintenance3", 1, 1, false},
    {Extension::ShaderFloatControls, "VK_KHR_shader_float_controls", 1, 2, true},
    {Extension::Spirv14, "VK_KHR_spirv_1_4", 1, 2, true},
    {Extension::DescriptorIndexing, "VK_EXT_descriptor_indexing", 1, 2, true},
    {Extension::BufferDeviceAddress, "VK_KHR_buffer_device_address", 1, 2, true},
    {Extension::TimelineSemaphore, "VK_KHR_timeline_semaphore", 1, 2, true},
    {Extension::ShaderFloat16Int8, "VK_KHR_shader_float16_int8", 1, 2, true},
    {Extension::ShaderSubgroupExtendedTypes, "VK_KHR_shader_subgroup_extended_types", 1, 2, true},
    {Extension::DrawIndirectCount, "VK_KHR_draw_indirect_count", 1, 2, true},
    {Extension::HostQueryReset, "VK_EXT_host_query_reset", 1, 2, true},
    {Extension::CreateRenderPass2, "VK_KHR_create_renderpass2", 1, 2, true},
    {Extension::DedicatedAllocation, "VK_KHR_dedicated_allocation", 1, 1, true},
    {Extension::DynamicRendering, "VK_KHR_dynamic_rendering", 1, 3, true},
    {Extension::Synchronization2, "VK_KHR_synchronization2", 1, 3, true},
    {Extension::InlineUniformBlock, "VK_EXT_inline_uniform_block", 1, 3, true},
    {Extension::ImageRobustness, "VK_EXT_image_robustness", 1, 3, true},
    {Extension::TextureCompressionASTCHDR, "VK_EXT_texture_compression_astc_hdr", 1, 3, true},
    {Extension::ExtendedDynamicState, "VK_EXT_extended_dynamic_state", 1, 3, true},
    {Extension::ExtendedDynamicState2, "VK_EXT_extended_dynamic_state2", 1, 3, true},
    {Extension::PushDescriptor, "VK_KHR_push_descriptor", 1, 4, true},
    {Extension::HostImageCopy, "VK_EXT_host_image_copy", 1, 4, true},
    {Extension::DynamicRenderingLocalRead, "VK_KHR_dynamic_rendering_local_read", 1, 4, true},

    // Optional, never core.
    {Extension::Swapchain, "VK_KHR_swapchain", 0, 0, true},
    {Extension::DescriptorBuffer, "VK_EXT_descriptor_buffer", 0, 0, true},
    {Extension::DescriptorHeap, "VK_EXT_descriptor_heap", 0, 0, true},
    {Extension::ShaderObject, "VK_EXT_shader_object", 0, 0, true},
    {Extension::GraphicsPipelineLibrary, "VK_EXT_graphics_pipeline_library", 0, 0, true},
    {Extension::PipelineLibrary, "VK_KHR_pipeline_library", 0, 0, true},
    {Extension::ExtendedDynamicState3, "VK_EXT_extended_dynamic_state3", 0, 0, true},
    {Extension::MeshShader, "VK_EXT_mesh_shader", 0, 0, true},
    {Extension::DeviceGeneratedCommands, "VK_EXT_device_generated_commands", 0, 0, true},
    {Extension::Maintenance5, "VK_KHR_maintenance5", 0, 0, true},
    {Extension::DeferredHostOperations, "VK_KHR_deferred_host_operations", 0, 0, true},
    {Extension::AccelerationStructure, "VK_KHR_acceleration_structure", 0, 0, true},
    {Extension::RayTracingPipeline, "VK_KHR_ray_tracing_pipeline", 0, 0, true},
    {Extension::RayQuery, "VK_KHR_ray_query", 0, 0, true},
    {Extension::FragmentShadingRate, "VK_KHR_fragment_shading_rate", 0, 0, true},
    {Extension::CooperativeMatrix, "VK_KHR_cooperative_matrix", 0, 0, true},
    {Extension::Robustness2, "VK_EXT_robustness2", 0, 0, true},
    {Extension::MemoryPriority, "VK_EXT_memory_priority", 0, 0, true},
    {Extension::PageableDeviceLocalMemory, "VK_EXT_pageable_device_local_memory", 0, 0, true},
    {Extension::MemoryBudget, "VK_EXT_memory_budget", 0, 0, true},
    {Extension::ShaderFramebufferFetch, "VK_EXT_shader_framebuffer_fetch", 0, 0, true},
    {Extension::PerformanceQuery, "VK_KHR_performance_query", 0, 0, false},
    {Extension::CalibratedTimestamps, "VK_KHR_calibrated_timestamps", 1, 4, false},
    {Extension::AndroidExternalMemoryHardwareBuffer, "VK_ANDROID_external_memory_android_hardware_buffer", 0, 0, true},

    // Video.
    {Extension::VideoQueue, "VK_KHR_video_queue", 0, 0, true},
    {Extension::VideoDecodeQueue, "VK_KHR_video_decode_queue", 0, 0, true},
    {Extension::VideoEncodeQueue, "VK_KHR_video_encode_queue", 0, 0, true},
    {Extension::VideoDecodeAv1, "VK_KHR_video_decode_av1", 0, 0, true},
    {Extension::VideoDecodeH265, "VK_KHR_video_decode_h265", 0, 0, true},
    {Extension::VideoDecodeH264, "VK_KHR_video_decode_h264", 0, 0, true},
    {Extension::VideoEncodeAv1, "VK_KHR_video_encode_av1", 0, 0, true},
    {Extension::VideoEncodeH265, "VK_KHR_video_encode_h265", 0, 0, true},
    {Extension::VideoEncodeH264, "VK_KHR_video_encode_h264", 0, 0, true},
    {Extension::VideoEncodeFeedback2, "VK_KHR_video_encode_feedback2", 0, 0, true},
};

const ExtensionInfo *FindExtensionInfo(Extension id)
{
    for (const auto &info : kExtensionInfo)
    {
        if (info.Id == id)
            return &info;
    }
    return nullptr;
}
} // namespace

bool VulkanExtensionCheck::Supports(Extension extension) const noexcept
{
    const ExtensionInfo *info = FindExtensionInfo(extension);
    if (info == nullptr)
        return false;

    if (info->CoreMajor != 0 && m_apiVersion >= VK_MAKE_API_VERSION(0, info->CoreMajor, info->CoreMinor, 0))
        return true;

    return IsAdvertised(info->Name);
}

std::vector<const char*> VulkanExtensionCheck::GetEnabledDeviceExtensions() const
{
    std::vector<const char*> extensions;

    for (const auto &info : kExtensionInfo)
    {
        if (!info.Enable)
            continue;

        // Swapchain is required for any presentable device, so it is requested
        // regardless of advertisement; everything else must be advertised.
        if (info.Id == Extension::Swapchain || IsAdvertised(info.Name))
            extensions.emplace_back(info.Name);
    }

    return extensions;
}

vk::PhysicalDeviceFeatures2* VulkanExtensionCheck::BuildDeviceFeatureChain()
{
    // ── Core Physical Device Features ──
    m_features2.features.samplerAnisotropy = VK_TRUE;
    m_features2.features.independentBlend = VK_TRUE;

    if (m_supportedCoreFeatures.shaderStorageImageWriteWithoutFormat)
        m_features2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    if (m_supportedCoreFeatures.shaderStorageImageReadWithoutFormat)
        m_features2.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    // Line polygon mode: the editor's wireframe view.
    if (m_supportedCoreFeatures.fillModeNonSolid)
        m_features2.features.fillModeNonSolid = VK_TRUE;

    // ── Always chain Vulkan 1.1 / 1.2 / 1.3 (they are core for Vulkan 1.3) ──
    m_features2.pNext = &m_v11Features;
    m_v11Features.pNext = &m_v12Features;
    m_v12Features.pNext = &m_v13Features;

    // ── Vulkan 1.1 Features ──
    if (Supports(Extension::ShaderDrawParameters))
        m_v11Features.shaderDrawParameters = VK_TRUE;
    if (Supports(Extension::Storage16Bit))
        m_v11Features.storageBuffer16BitAccess = VK_TRUE;
    if (Supports(Extension::Multiview))
        m_v11Features.multiview = VK_TRUE;
    if (Supports(Extension::SamplerYcbcrConversion))
        m_v11Features.samplerYcbcrConversion = VK_TRUE;

    // ── Vulkan 1.2 Features ──
    if (Supports(Extension::DescriptorIndexing))
    {
        m_v12Features.descriptorIndexing = VK_TRUE;
        m_v12Features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        m_v12Features.descriptorBindingPartiallyBound = VK_TRUE;
        m_v12Features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        m_v12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        m_v12Features.runtimeDescriptorArray = VK_TRUE;
    }
    if (Supports(Extension::BufferDeviceAddress))
        m_v12Features.bufferDeviceAddress = VK_TRUE;
    if (Supports(Extension::TimelineSemaphore))
        m_v12Features.timelineSemaphore = VK_TRUE;
    if (Supports(Extension::ShaderFloat16Int8))
    {
        m_v12Features.shaderFloat16 = VK_TRUE;
        m_v12Features.shaderInt8 = VK_TRUE;
    }
    if (Supports(Extension::ShaderSubgroupExtendedTypes))
        m_v12Features.shaderSubgroupExtendedTypes = VK_TRUE;
    if (Supports(Extension::DrawIndirectCount))
        m_v12Features.drawIndirectCount = VK_TRUE;
    if (Supports(Extension::HostQueryReset))
        m_v12Features.hostQueryReset = VK_TRUE;

    // ── Vulkan 1.3 Features ──
    if (Supports(Extension::DynamicRendering))
        m_v13Features.dynamicRendering = VK_TRUE;
    if (Supports(Extension::Synchronization2))
        m_v13Features.synchronization2 = VK_TRUE;
    if (Supports(Extension::InlineUniformBlock))
        m_v13Features.inlineUniformBlock = VK_TRUE;
    if (Supports(Extension::ImageRobustness))
        m_v13Features.robustImageAccess = VK_TRUE;
    // The extension is core at 1.3, but the FEATURE BIT is optional hardware
    // capability (RADV Renoir reports false). Gate on the queried bit, not the
    // extension/API version — requesting an unsupported bit kills
    // vkCreateDevice with FeatureNotPresent.
    if (m_supportedV13Features.textureCompressionASTC_HDR)
        m_v13Features.textureCompressionASTC_HDR = VK_TRUE;

    // ── Extension Feature Structs (appended to the pNext chain after v13) ──
    void** pNextChainTail = &m_v13Features.pNext;

    // Core at 1.3, but the feature struct stays the EXT one.
    if (Supports(Extension::ExtendedDynamicState))
    {
        m_dynamicStateFeatures.extendedDynamicState = VK_TRUE;
        *pNextChainTail = &m_dynamicStateFeatures;
        pNextChainTail = &m_dynamicStateFeatures.pNext;
    }
    if (Supports(Extension::ExtendedDynamicState2))
    {
        m_dynamicState2Features.extendedDynamicState2 = VK_TRUE;
        *pNextChainTail = &m_dynamicState2Features;
        pNextChainTail = &m_dynamicState2Features.pNext;
    }
    if (Supports(Extension::DescriptorBuffer))
    {
        m_descriptorBufferFeatures.descriptorBuffer = VK_TRUE;
        *pNextChainTail = &m_descriptorBufferFeatures;
        pNextChainTail = &m_descriptorBufferFeatures.pNext;
    }
    if (Supports(Extension::ShaderObject))
    {
        m_shaderObjectFeatures.shaderObject = VK_TRUE;
        *pNextChainTail = &m_shaderObjectFeatures;
        pNextChainTail = &m_shaderObjectFeatures.pNext;
    }
    if (Supports(Extension::GraphicsPipelineLibrary))
    {
        m_graphicsPipelineLibraryFeatures.graphicsPipelineLibrary = VK_TRUE;
        *pNextChainTail = &m_graphicsPipelineLibraryFeatures;
        pNextChainTail = &m_graphicsPipelineLibraryFeatures.pNext;
    }
    if (Supports(Extension::MeshShader))
    {
        m_meshShaderFeatures.meshShader = VK_TRUE;
        m_meshShaderFeatures.taskShader = VK_TRUE;
        *pNextChainTail = &m_meshShaderFeatures;
        pNextChainTail = &m_meshShaderFeatures.pNext;
    }
    if (Supports(Extension::AccelerationStructure))
    {
        m_accelerationStructureFeatures.accelerationStructure = VK_TRUE;
        *pNextChainTail = &m_accelerationStructureFeatures;
        pNextChainTail = &m_accelerationStructureFeatures.pNext;
    }
    if (Supports(Extension::RayTracingPipeline))
    {
        m_rayTracingPipelineFeatures.rayTracingPipeline = VK_TRUE;
        *pNextChainTail = &m_rayTracingPipelineFeatures;
        pNextChainTail = &m_rayTracingPipelineFeatures.pNext;
    }
    if (Supports(Extension::RayQuery))
    {
        m_rayQueryFeatures.rayQuery = VK_TRUE;
        *pNextChainTail = &m_rayQueryFeatures;
        pNextChainTail = &m_rayQueryFeatures.pNext;
    }
    if (Supports(Extension::FragmentShadingRate))
    {
        m_fragmentShadingRateFeatures.pipelineFragmentShadingRate =
            m_supportedFragmentShadingRateFeatures.pipelineFragmentShadingRate;
        m_fragmentShadingRateFeatures.primitiveFragmentShadingRate =
            m_supportedFragmentShadingRateFeatures.primitiveFragmentShadingRate;
        m_fragmentShadingRateFeatures.attachmentFragmentShadingRate =
            m_supportedFragmentShadingRateFeatures.attachmentFragmentShadingRate;
        *pNextChainTail = &m_fragmentShadingRateFeatures;
        pNextChainTail = &m_fragmentShadingRateFeatures.pNext;
    }
    if (Supports(Extension::CooperativeMatrix))
    {
        m_cooperativeMatrixFeatures.cooperativeMatrix = VK_TRUE;
        *pNextChainTail = &m_cooperativeMatrixFeatures;
        pNextChainTail = &m_cooperativeMatrixFeatures.pNext;
    }
    if (Supports(Extension::Robustness2))
    {
        m_features2.features.robustBufferAccess = VK_TRUE;
        m_robustness2Features.robustBufferAccess2 = VK_TRUE;
        m_robustness2Features.robustImageAccess2 = VK_TRUE;
        m_robustness2Features.nullDescriptor = VK_TRUE;
        *pNextChainTail = &m_robustness2Features;
        pNextChainTail = &m_robustness2Features.pNext;
    }
    if (Supports(Extension::MemoryPriority))
    {
        m_memoryPriorityFeatures.memoryPriority = VK_TRUE;
        *pNextChainTail = &m_memoryPriorityFeatures;
        pNextChainTail = &m_memoryPriorityFeatures.pNext;
    }
    if (Supports(Extension::PageableDeviceLocalMemory))
    {
        m_pageableDeviceLocalMemoryFeatures.pageableDeviceLocalMemory = VK_TRUE;
        *pNextChainTail = &m_pageableDeviceLocalMemoryFeatures;
        pNextChainTail = &m_pageableDeviceLocalMemoryFeatures.pNext;
    }
    // Same optional-bit rule as ASTC-HDR: core promotion makes the struct
    // queryable, the bit itself stays hardware-optional.
    if (m_supportedHostImageCopyFeatures.hostImageCopy)
    {
        m_hostImageCopyFeatures.hostImageCopy = VK_TRUE;
        *pNextChainTail = &m_hostImageCopyFeatures;
        pNextChainTail = &m_hostImageCopyFeatures.pNext;
    }
    if (Supports(Extension::DynamicRenderingLocalRead))
    {
        m_dynamicRenderingLocalReadFeatures.dynamicRenderingLocalRead = VK_TRUE;
        *pNextChainTail = &m_dynamicRenderingLocalReadFeatures;
        pNextChainTail = &m_dynamicRenderingLocalReadFeatures.pNext;
    }

    *pNextChainTail = nullptr;
    return &m_features2;
};

void VulkanExtensionCheck::QuerySupportedFeatures(const vk::raii::PhysicalDevice& PhysicalDevice)
{
    vk::PhysicalDeviceFeatures2 coreQuery{};
    vkGetPhysicalDeviceFeatures2(*PhysicalDevice, reinterpret_cast<VkPhysicalDeviceFeatures2*>(&coreQuery));
    m_supportedCoreFeatures = coreQuery.features;

    // ── Query the optional feature structs the feature chain wants to enable ──
    // Promotion folds the STRUCT into core (always queryable), not the BITS
    // (still optional). Asking vkCreateDevice for a false bit is
    // VK_ERROR_FEATURE_NOT_PRESENT — the RADV Renoir class of failure — so
    // BuildDeviceFeatureChain gates on these queried copies.
    vk::PhysicalDeviceFeatures2 queryChain{};
    m_supportedV13Features.pNext = &m_supportedHostImageCopyFeatures;
    m_supportedHostImageCopyFeatures.pNext = nullptr;
    queryChain.pNext = &m_supportedV13Features;
    vkGetPhysicalDeviceFeatures2(*PhysicalDevice, reinterpret_cast<VkPhysicalDeviceFeatures2*>(&queryChain));

    if (!Supports(Extension::FragmentShadingRate))
        return;

    vk::PhysicalDeviceFeatures2 queryFeatures2;
    m_supportedFragmentShadingRateFeatures.pNext = nullptr;
    queryFeatures2.pNext = &m_supportedFragmentShadingRateFeatures;
    vkGetPhysicalDeviceFeatures2(*PhysicalDevice, reinterpret_cast<VkPhysicalDeviceFeatures2*>(&queryFeatures2));
}

void VulkanExtensionCheck::TickTheAvailableExtension(const vk::raii::PhysicalDevice& PhysicalDevice)
{
    uint32_t propertyCount = 0;
    vkEnumerateDeviceExtensionProperties(*PhysicalDevice, nullptr, &propertyCount, nullptr);
    std::vector<VkExtensionProperties> properties(propertyCount);
    vkEnumerateDeviceExtensionProperties(*PhysicalDevice, nullptr, &propertyCount, properties.data());

    m_advertised.clear();
    m_advertised.reserve(properties.size());
    for (const auto &ext : properties)
        m_advertised.emplace_back(ext.extensionName);

    m_apiVersion = PhysicalDevice.getProperties().apiVersion;

    if (!SupportsSync2())
        m_syncTier = SyncTier::Legacy;
    else
        m_syncTier = SupportsTimelineSemaphores() ? SyncTier::Sync2Timeline : SyncTier::Sync2;
}

} // namespace UHE::RHI::VULKAN
