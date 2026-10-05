#pragma once
/**
 * \file VulkanContext.h
 * \brief Process-wide handle bag for the live Vulkan backend.
 *
 * The context is deliberately a flat struct rather than a set of singletons so the
 * ownership graph stays obvious: whoever creates the context (VulkanDevice) owns
 * every pointer in it. Helper code reaches it through GetVulkanContext().
 *
 * \warning The pointed-to objects are non-owning references. Do not delete through
 *          the context and do not outlive the device that created it.
 */
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/Core/Core.h"

namespace UHE::RHI::VULKAN
{

class VulkanInstance;
class VulkanPhysicalDevice;
class VulkanLogicalDevice;
class VulkanSwapChain;
class VulkanDevice;
class VulkanDescriptorManager;
class VulkanExtensionCheck;
class VulkanGraphicPipeline;
class VulkanDescriptorPool;

/// Non-owning bundle of the objects that make up a running Vulkan backend.
struct VulkanContext
{
    VulkanInstance* instance = nullptr;
    VulkanPhysicalDevice* physicalDevice = nullptr;
    VulkanLogicalDevice* logicalDevice = nullptr;
    VulkanExtensionCheck* CheckExtensions = nullptr;
    VulkanSwapChain* swapChain = nullptr;
    VulkanDevice* device = nullptr;
    VulkanDescriptorManager* descriptorManager = nullptr;
    VulkanGraphicPipeline* graphicPipeline = nullptr;
    VulkanDescriptorPool* fallbackDescriptorPool = nullptr;

    VmaAllocator allocator = nullptr; ///< Vulkan Memory Allocator instance.
    vk::raii::Device* logicalDeviceHandle = nullptr;
    vk::raii::PhysicalDevice* physicalDeviceHandle = nullptr;
    vk::raii::Instance* instanceHandle = nullptr;
    vk::raii::Queue* graphicsQueue = nullptr;
    vk::raii::SurfaceKHR* surface = nullptr;

    u32 graphicsQueueFamilyIndex = 0;
    u32 currentFrameIndex = 0;
    u32 imageIndex = 0;
};

/// Current context, or nullptr before the device is up. Set by VulkanDevice.
extern VulkanContext* g_VulkanContext;

/// \returns the live context. Only valid while a VulkanDevice exists.
/// \see VulkanUtils.cpp for the definition of g_VulkanContext.
inline VulkanContext& GetVulkanContext()
{
    return *g_VulkanContext;
}

} // namespace UHE::RHI::VULKAN
