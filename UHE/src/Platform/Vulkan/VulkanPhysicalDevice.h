#pragma once
#include <optional>
#include <vulkan/vulkan_raii.hpp>
#include "VulkanInstance.h"
#include "vulkan/vulkan.hpp"

namespace UHE::RHI::VULKAN
{
/// Selected queue family indices for the physical device.
struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsFamily;
    [[nodiscard]] bool isComplete() const { return graphicsFamily.has_value(); }
};

/**
 * \brief Picks and wraps the physical device (GPU) the backend will use.
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/vkEnumeratePhysicalDevices.html
 */
class VulkanPhysicalDevice
{
public:
    VulkanPhysicalDevice() = default;
    VulkanPhysicalDevice(const VulkanPhysicalDevice&) = delete;
    VulkanPhysicalDevice& operator=(const VulkanPhysicalDevice&) = delete;

    void initPhysicalDevice(VulkanInstance& instance);
    [[nodiscard]] vk::raii::PhysicalDevice& getPhysicalDevice() { return m_physicalDevice; }
    [[nodiscard]] const QueueFamilyIndices& getQueueFamilyIndices() const { return m_queueFamilyIndices; }
    /// Scores a candidate device (higher is better); used to pick among suitable GPUs.
    [[nodiscard]] i32 RateDevice(const vk::raii::PhysicalDevice& device) const;
    [[nodiscard]] bool IsDeviceSuitable(const vk::raii::PhysicalDevice& device) const;
    void GetLogicalDeviceInfo(u32& vendorID, u32& deviceID) const
    {
        vendorID = m_physicalDevice.getProperties().vendorID;
        deviceID = m_physicalDevice.getProperties().deviceID;
    }

private:
    vk::PhysicalDevice FindPhysicalDevice();

private:
    vk::raii::PhysicalDevice m_physicalDevice = nullptr;
    QueueFamilyIndices m_queueFamilyIndices;
    std::vector<const char*> requiredDeviceExtension = {
        vk::KHRSwapchainExtensionName,           vk::KHRSpirv14ExtensionName,
        vk::KHRSynchronization2ExtensionName,    vk::KHRCreateRenderpass2ExtensionName,
        vk::KHRShaderFloatControlsExtensionName, vk::KHRDynamicRenderingExtensionName};
};
} // namespace UHE::RHI::VULKAN
