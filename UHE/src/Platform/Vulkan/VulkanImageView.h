#pragma once
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>

namespace UHE::RHI::VULKAN
{
class VulkanDevice;
class VulkanImageView
{
public:
    VulkanImageView() = default;
    VulkanImageView(const VulkanImageView&) = delete;
    VulkanImageView& operator=(const VulkanImageView&) = delete;

    ~VulkanImageView();

    vk::raii::ImageView CreateImageView(vk::Image image, vk::Format format);

    void setPyhsicalDevice(vk::raii::PhysicalDevice& physicalDevice) { this->physicaldevice = &physicalDevice; }
    void LogicalDevice(vk::raii::Device& logicalDevice) { this->logicaldevice = &logicalDevice; }
    void setSwapChainSurfaceFormat(vk::SurfaceFormatKHR& swapChainSurfaceFormat)
    {
        this->swapChainSurfaceFormat = &swapChainSurfaceFormat;
    }

    void CreateImageViews();
    void CreateTextureImageView();
    void CreateTextureSampler();
    void CreateDepthImageView(vk::Extent2D swapChainExtent, vk::Format depthFormat = vk::Format::eD32Sfloat);

    [[nodiscard]] vk::ImageView GetImageView() { return *textureImageView; }
    [[nodiscard]] vk::ImageView GetDepthImageView() { return *depthImageView; }
    [[nodiscard]] vk::Sampler GetTextureSampler() { return *textureSampler; }

private:
    vk::raii::PhysicalDevice* physicaldevice = nullptr;
    vk::raii::Device* logicaldevice = nullptr;
    vk::SurfaceFormatKHR* swapChainSurfaceFormat = nullptr;
    std::vector<vk::raii::ImageView> swapChainImageViews;
    std::vector<vk::Image> swapChainImages;
    vk::raii::ImageView textureImageView;
    vk::raii::Sampler textureSampler;
    vk::raii::ImageView depthImageView;
    vk::Image rawHandle = nullptr;
    VkImage rawImage = nullptr;
    vk::raii::Image depthImage;
    VmaAllocator m_allocator = nullptr;
    VmaAllocation depthImageAllocation = nullptr;
};
} // namespace UHE::RHI::VULKAN
