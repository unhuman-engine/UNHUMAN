#define VK_NO_PROTOTYPES
#include "uhepch.h"
#include "VulkanLogicalDevice.h"
#include <volk.h>
#include "VulkanExtensionCheck.h"
#include "VulkanInstance.h"
#include "VulkanPhysicalDevice.h"
namespace UHE::RHI::VULKAN
{
void VulkanLogicalDevice::initialize(VulkanPhysicalDevice& physicalDevice, VkSurfaceKHR surface,
                                     VulkanInstance& instance, VulkanExtensionCheck& CheckExtens)
{
    const auto& phyDevice = physicalDevice.getPhysicalDevice();
    std::vector<vk::QueueFamilyProperties> queueFamilies = phyDevice.getQueueFamilyProperties();
    m_graphicsQueueFamilyIndex = static_cast<u32>(-1);
    for (size_t i = 0; i < queueFamilies.size(); i++)
    {
        if (queueFamilies[i].queueFlags & vk::QueueFlagBits::eGraphics)
        {
            // Check if the queue family supports presentation to the surface
            if (phyDevice.getSurfaceSupportKHR(i, surface))
            {
                // Store the index of the graphics and presentation queue family
                m_graphicsQueueFamilyIndex = i;
                break;
            }
        }
    }
    if (queueFamilies.empty())
    {
        throw std::runtime_error("Failed to find a suitable queue family!");
    }

    CheckExtens.TickTheAvailableExtension(phyDevice);
    CheckExtens.QuerySupportedFeatures(phyDevice);
    auto deviceExtensions = CheckExtens.GetEnabledDeviceExtensions();

    // Dedicated queues (issue #30): a compute family without graphics runs
    // async compute next to the frame; a transfer-only family runs uploads on
    // the copy engine. A GPU with neither (lavapipe, most mobile) does that
    // work on the graphics queue, which is what the getters then return.
    const auto findFamily = [&](vk::QueueFlags want, vk::QueueFlags avoid) -> u32
    {
        for (u32 i = 0; i < queueFamilies.size(); ++i)
            if ((queueFamilies[i].queueFlags & want) == want && !(queueFamilies[i].queueFlags & avoid) &&
                queueFamilies[i].queueCount > 0)
                return i;
        return static_cast<u32>(-1);
    };
    m_computeQueueFamilyIndex = findFamily(vk::QueueFlagBits::eCompute, vk::QueueFlagBits::eGraphics);
    m_transferQueueFamilyIndex =
        findFamily(vk::QueueFlagBits::eTransfer, vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute);

    float queuePriority = 1.0f;
    std::vector<vk::DeviceQueueCreateInfo> queueInfos;
    for (const u32 family : {m_graphicsQueueFamilyIndex, m_computeQueueFamilyIndex, m_transferQueueFamilyIndex})
        if (family != static_cast<u32>(-1))
            queueInfos.push_back(vk::DeviceQueueCreateInfo{
                .flags = {}, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &queuePriority});

    vk::DeviceCreateInfo deviceCreateInfo{.pNext = CheckExtens.BuildDeviceFeatureChain(),
                                          .flags = {},
                                          .queueCreateInfoCount = static_cast<u32>(queueInfos.size()),
                                          .pQueueCreateInfos = queueInfos.data(),
                                          .enabledLayerCount = 0,
                                          .ppEnabledLayerNames = nullptr,
                                          .enabledExtensionCount = static_cast<u32>(deviceExtensions.size()),
                                          .ppEnabledExtensionNames = deviceExtensions.data(),
                                          .pEnabledFeatures = nullptr};

    m_logicalDevice = vk::raii::Device(phyDevice, deviceCreateInfo);
    volkLoadDevice(*m_logicalDevice);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*instance.getInstance(), *m_logicalDevice);
    m_graphicsQueue = vk::raii::Queue(m_logicalDevice, m_graphicsQueueFamilyIndex, 0);
    if (m_computeQueueFamilyIndex != static_cast<u32>(-1))
        m_computeQueue = vk::raii::Queue(m_logicalDevice, m_computeQueueFamilyIndex, 0);
    if (m_transferQueueFamilyIndex != static_cast<u32>(-1))
        m_transferQueue = vk::raii::Queue(m_logicalDevice, m_transferQueueFamilyIndex, 0);

    VmaVulkanFunctions vulkanFunctions{.vkGetInstanceProcAddr = vkGetInstanceProcAddr,
                                       .vkGetDeviceProcAddr = vkGetDeviceProcAddr};

    VmaAllocatorCreateInfo allocatorCreateInfo{.flags = 0,
                                               .physicalDevice = *physicalDevice.getPhysicalDevice(),
                                               .device = *m_logicalDevice,
                                               .preferredLargeHeapBlockSize = 0,
                                               .pAllocationCallbacks = nullptr,
                                               .pDeviceMemoryCallbacks = nullptr,
                                               .pHeapSizeLimit = nullptr,
                                               .pVulkanFunctions = &vulkanFunctions,
                                               .instance = *instance.getInstance(),
                                               .vulkanApiVersion = VK_API_VERSION_1_3};

    if (vmaCreateAllocator(&allocatorCreateInfo, &m_allocator) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create VMA allocator!");
    }
}

void VulkanLogicalDevice::CreateSurface(VulkanInstance& instance, GLFWwindow* window)
{
    auto const& m_instance = instance.getInstance();

    VkSurfaceKHR _surface;
    if (glfwCreateWindowSurface(*m_instance, window, nullptr, &_surface) != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to create window surface!");
    }
    surface = vk::raii::SurfaceKHR(m_instance, _surface);
}

void VulkanLogicalDevice::cleanup()
{
    vmaDestroyAllocator(m_allocator);
    m_logicalDevice = nullptr;
}
} // namespace UHE::RHI::VULKAN
