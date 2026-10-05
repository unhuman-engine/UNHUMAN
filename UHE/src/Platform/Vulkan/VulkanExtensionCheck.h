#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace UHE::RHI::VULKAN
{
// How synchronization is expressed on this device, chosen once from the device
// API version and the extensions it advertises. Higher layers branch on the tier
// rather than on extension strings.
enum class SyncTier
{
    Legacy = 0,   // Vulkan 1.1: vkCmdPipelineBarrier, vkQueueSubmit, fences
    Sync2,        // Vulkan 1.3 or VK_KHR_synchronization2
    Sync2Timeline // Sync2 plus timeline semaphores (Vulkan 1.2 or extension)
};

/// Human-readable name for logging and diagnostics.
[[nodiscard]] const char* SyncTierName(SyncTier tier);

// Every extension the engine knows about. Each maps to a name and the core
// version it was promoted to (0, 0 means never promoted). This is the single
// place an extension is declared; the table in the .cpp is the only place its
// name appears as a string.
enum class Extension : uint8_t
{
    // Promoted to core.
    ShaderDrawParameters,
    Storage16Bit,
    Multiview,
    SamplerYcbcrConversion,
    Maintenance1,
    Maintenance2,
    Maintenance3,
    ShaderFloatControls,
    Spirv14,
    DescriptorIndexing,
    BufferDeviceAddress,
    TimelineSemaphore,
    ShaderFloat16Int8,
    ShaderSubgroupExtendedTypes,
    DrawIndirectCount,
    HostQueryReset,
    CreateRenderPass2,
    DedicatedAllocation,
    DynamicRendering,
    Synchronization2,
    InlineUniformBlock,
    ImageRobustness,
    TextureCompressionASTCHDR,
    ExtendedDynamicState,
    ExtendedDynamicState2,
    PushDescriptor,
    HostImageCopy,
    DynamicRenderingLocalRead,

    // Optional, never core.
    Swapchain,
    DescriptorBuffer,
    DescriptorHeap,
    ShaderObject,
    GraphicsPipelineLibrary,
    PipelineLibrary,
    ExtendedDynamicState3,
    MeshShader,
    DeviceGeneratedCommands,
    Maintenance5,
    DeferredHostOperations,
    AccelerationStructure,
    RayTracingPipeline,
    RayQuery,
    FragmentShadingRate,
    CooperativeMatrix,
    Robustness2,
    MemoryPriority,
    PageableDeviceLocalMemory,
    MemoryBudget,
    ShaderFramebufferFetch,
    PerformanceQuery,
    CalibratedTimestamps,
    AndroidExternalMemoryHardwareBuffer,

    // Video.
    VideoQueue,
    VideoDecodeQueue,
    VideoEncodeQueue,
    VideoDecodeAv1,
    VideoDecodeH265,
    VideoDecodeH264,
    VideoEncodeAv1,
    VideoEncodeH265,
    VideoEncodeH264,
    VideoEncodeFeedback2,
};

/**
 * \brief Queries and caches the physical device's capabilities and picks a sync tier.
 *
 * "Supports X" means the capability is usable — a core feature of the device's API
 * version or an advertised extension — not merely that an extension string exists.
 * Higher layers branch on SyncTier rather than poking extension strings directly.
 *
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VK_API_VERSION_1_0.html
 * \see https://www.khronos.org/blog/vulkan-timeline-semaphores
 */
class VulkanExtensionCheck
{
public:
    VulkanExtensionCheck() = default;
    ~VulkanExtensionCheck() = default;

    // True when the device advertises the extension, independent of whether the
    // engine enables it.
    [[nodiscard]] bool IsAdvertised(std::string_view name) const noexcept
    {
        for (const auto& advertised : m_advertised)
        {
            if (std::string_view(advertised) == name)
                return true;
        }
        return false;
    }

    // True when the capability is usable: either a core feature of the device's
    // API version, or an advertised extension. Checking only the extension name
    // is wrong on drivers that do not advertise promoted extensions.
    [[nodiscard]] bool Supports(Extension extension) const noexcept;

    [[nodiscard]] bool SupportsSync2() const noexcept { return Supports(Extension::Synchronization2); }
    [[nodiscard]] bool SupportsTimelineSemaphores() const noexcept { return Supports(Extension::TimelineSemaphore); }
    [[nodiscard]] bool SupportsDynamicRendering() const noexcept { return Supports(Extension::DynamicRendering); }
    [[nodiscard]] SyncTier GetSyncTier() const noexcept { return m_syncTier; }

    // Extension strings to enable, built from the table: every extension marked
    // as enabled that this device advertises.
    /// Extension strings to enable: every enabled extension this device advertises.
    [[nodiscard]] std::vector<const char*> GetEnabledDeviceExtensions() const;
    /// Builds the pNext chain of feature structs to enable at vkCreateDevice time.
    [[nodiscard]] vk::PhysicalDeviceFeatures2* BuildDeviceFeatureChain();
    /// Records the extensions \p PhysicalDevice advertises.
    void TickTheAvailableExtension(const vk::raii::PhysicalDevice& PhysicalDevice);
    /// Queries which optional feature bits the hardware actually reports as true.
    void QuerySupportedFeatures(const vk::raii::PhysicalDevice& PhysicalDevice);

private:
    uint32_t m_apiVersion = 0;
    SyncTier m_syncTier = SyncTier::Legacy;
    std::vector<std::string> m_advertised;
    vk::PhysicalDeviceFeatures2 m_features2;
    vk::PhysicalDeviceFeatures m_supportedCoreFeatures;
    vk::PhysicalDeviceVulkan11Features m_v11Features;
    vk::PhysicalDeviceVulkan12Features m_v12Features;
    vk::PhysicalDeviceVulkan13Features m_v13Features;
    // ── What the HARDWARE actually supports (queried, never assumed) ──
    // "Promoted to core" only means the EXT struct is always available to
    // query — individual feature bits inside it stay optional. Enabling a
    // feature the device reports false for fails vkCreateDevice with
    // FeatureNotPresent (e.g. RADV Renoir: no ASTC-HDR, no hostImageCopy),
    // so feature-struct enables must gate on these queried bits.
    vk::PhysicalDeviceVulkan13Features m_supportedV13Features;
    vk::PhysicalDeviceHostImageCopyFeaturesEXT m_supportedHostImageCopyFeatures;
    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT m_dynamicStateFeatures;
    vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT m_dynamicState2Features;
    vk::PhysicalDeviceDescriptorBufferFeaturesEXT m_descriptorBufferFeatures;
    vk::PhysicalDeviceShaderObjectFeaturesEXT m_shaderObjectFeatures;
    vk::PhysicalDeviceGraphicsPipelineLibraryFeaturesEXT m_graphicsPipelineLibraryFeatures;
    vk::PhysicalDeviceMeshShaderFeaturesEXT m_meshShaderFeatures;
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR m_accelerationStructureFeatures;
    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR m_rayTracingPipelineFeatures;
    vk::PhysicalDeviceRayQueryFeaturesKHR m_rayQueryFeatures;
    vk::PhysicalDeviceFragmentShadingRateFeaturesKHR m_fragmentShadingRateFeatures;
    vk::PhysicalDeviceFragmentShadingRateFeaturesKHR m_supportedFragmentShadingRateFeatures;
    vk::PhysicalDeviceCooperativeMatrixFeaturesKHR m_cooperativeMatrixFeatures;
    vk::PhysicalDeviceRobustness2FeaturesEXT m_robustness2Features;
    vk::PhysicalDeviceMemoryPriorityFeaturesEXT m_memoryPriorityFeatures;
    vk::PhysicalDevicePageableDeviceLocalMemoryFeaturesEXT m_pageableDeviceLocalMemoryFeatures;
    vk::PhysicalDeviceHostImageCopyFeaturesEXT m_hostImageCopyFeatures;
    vk::PhysicalDeviceDynamicRenderingLocalReadFeaturesKHR m_dynamicRenderingLocalReadFeatures;
};

} // namespace UHE::RHI::VULKAN
