#include "uhepch.h"
#include "VulkanPhysicalDevice.h"
#include <stdexcept>
#include <vulkan/vulkan_raii.hpp>

namespace UHE::RHI::VULKAN
{
void VulkanPhysicalDevice::initPhysicalDevice(VulkanInstance& m_Vkinstance)
{
    auto const& m_instance = m_Vkinstance.getInstance();
    std::vector<vk::raii::PhysicalDevice> devices = m_instance.enumeratePhysicalDevices();
    if (devices.empty())
    {
        throw std::runtime_error("Failed to find a GPU with Vulkan support!");
    }
    auto bestDevice = devices.end();
    i32 bestScore = -1;

    for (auto it = devices.begin(); it != devices.end(); ++it)
    {
        bool suitable = IsDeviceSuitable(*it);
        if (!suitable)
            continue;

        i32 score = RateDevice(*it);
        if (score > bestScore)
        {
            bestScore = score;
            bestDevice = it;
        }
    }

    if (bestDevice == devices.end())
    {
        throw std::runtime_error("Failed to find suitable GPU with Vulkan support!");
    }

    m_physicalDevice = *bestDevice;

    auto props = m_physicalDevice.getProperties();
    UHE_CORE_INFO("Selected GPU: {}", props.deviceName.data());
    UHE_CORE_INFO("Vendor ID: {}", props.vendorID);
    UHE_CORE_INFO("API Version: {}.{}.{}", VK_API_VERSION_MAJOR(props.apiVersion),
                  VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    // Populate m_queueFamilyIndices for the chosen device
    auto queueFamilies = m_physicalDevice.getQueueFamilyProperties();
    int i = 0;
    for (const auto& qfp : queueFamilies)
    {
        if (qfp.queueFlags & vk::QueueFlagBits::eGraphics)
        {
            m_queueFamilyIndices.graphicsFamily = i;
            break; // Found a graphics family
        }
        i++;
    }
}

bool VulkanPhysicalDevice::IsDeviceSuitable(const vk::raii::PhysicalDevice& device) const
{
    bool supportsVulkan1_3 = device.getProperties().apiVersion >= VK_API_VERSION_1_3;
    auto queueFamilies = device.getQueueFamilyProperties();
    bool supportsGraphics = false;
    for (const auto& qfp : queueFamilies)
    {
        if (qfp.queueFlags & vk::QueueFlagBits::eGraphics)
        {
            supportsGraphics = true;
            break;
        }
    }

    auto availableExtensions = device.enumerateDeviceExtensionProperties();
    bool supportsAllRequiredExtensions = std::ranges::all_of(
        requiredDeviceExtension,
        [&availableExtensions](const char* requiredDeviceExtensionName)
        {
            return std::ranges::any_of(
                availableExtensions, [&requiredDeviceExtensionName](const auto& extensionProperties)
                { return strcmp(extensionProperties.extensionName, requiredDeviceExtensionName) == 0; });
        });

    auto features =
        device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features,
                            vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();

    // The global descriptor set is bindless (runtime arrays, update-after-bind,
    // partially bound) and every shader indexes it; there is no per-draw set
    // fallback yet, so a device without these features cannot render at all.
    // @todo #13: a legacy descriptor path, then relax this check.
    const auto& v12 = features.get<vk::PhysicalDeviceVulkan12Features>();
    const bool supportsBindless =
        v12.descriptorIndexing && v12.runtimeDescriptorArray && v12.descriptorBindingPartiallyBound &&
        v12.descriptorBindingStorageBufferUpdateAfterBind && v12.descriptorBindingSampledImageUpdateAfterBind &&
        v12.shaderSampledImageArrayNonUniformIndexing;
    if (!supportsBindless)
    {
        UHE_CORE_WARN("Vulkan: skipping '{}': no bindless descriptor indexing, which the renderer requires",
                      device.getProperties().deviceName.data());
    }

    bool supportsRequiredFeatures =
        supportsBindless && features.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering &&
        features.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>().extendedDynamicState;

    return supportsVulkan1_3 && supportsGraphics && supportsAllRequiredExtensions && supportsRequiredFeatures;
}

i32 VulkanPhysicalDevice::RateDevice(const vk::raii::PhysicalDevice& device) const
{
    auto Props = device.getProperties();
    int score = 0;

    switch (Props.deviceType)
    {
        case vk::PhysicalDeviceType::eDiscreteGpu:
            score += 10000;
            break;
        case vk::PhysicalDeviceType::eIntegratedGpu:
            score += 5000;
            break;
        case vk::PhysicalDeviceType::eVirtualGpu:
            score += 1000;
            break;
        default:
            break;
    }

    auto memoryProps = device.getMemoryProperties();
    // may this not even need but just for choose GPU that have more VRAM
    for (const auto& heap : memoryProps.memoryHeaps)
    {
        if (heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal)
        {
            score += static_cast<i32>(static_cast<int>(heap.size) / (1024 * 1024 * 1024)); // GB
        }
    }
    return score;
}

} // namespace UHE::RHI::VULKAN
