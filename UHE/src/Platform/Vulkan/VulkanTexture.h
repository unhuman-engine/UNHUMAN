#pragma once
#include <vector>
#include <span>
#include <cstddef>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHITexture.h"

namespace UHE::RHI::VULKAN
{
class VulkanDevice;
class VulkanLogicalDevice;

class VulkanTexture final : public RHITexture
{
public:
    VulkanTexture() = default;
    ~VulkanTexture() override;

    const TextureDesc& GetDesc() const override { return m_Desc; }
    void* GetImGuiTextureID() override;
    u32 GetTextureIndex() const override { return m_TextureIndex; }

    void Init(VulkanDevice& device, const TextureDesc& desc);
    void CreateImage(VulkanLogicalDevice& device, uint32_t width, uint32_t height, uint32_t mipLevels, vk::Format format,
                     vk::ImageUsageFlags usage, VmaMemoryUsage memUsage, vk::ImageTiling tiling, vk::Image& image,
                     VmaAllocation& imageMemory);
    void CreateTexture(VulkanDevice& device, const void* pixelData, u32 width, u32 height, size_t dataSize);

    // Sampler state for the next CreateTexture call. The backend default matches
    // what it always used (linear/linear, repeat on all axes), so a texture
    // loaded without calling this renders unchanged.
    void SetSamplerDesc(const SamplerDesc& desc) { m_SamplerDesc = desc; }
    void ExecuteCopyCommand(VulkanDevice& device, VkBuffer srcBuffer, vk::Image dstImage, uint32_t width,
                            uint32_t height, uint32_t mipLevels);
    void GenerateMipmaps(VulkanDevice& device, vk::Image image, vk::Format imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels);
    void UpdateTexture(std::span<const u8> data);
    vk::Image& GetImage() { return textureImage; }
    vk::raii::ImageView& GetImageView() { return textureImageView; }
    vk::raii::Sampler& GetSampler() { return textureSampler; }

private:
    vk::Image textureImage{nullptr};
    VmaAllocation textureImageMemory = nullptr;
    vk::raii::ImageView textureImageView{nullptr};
    vk::raii::Sampler textureSampler{nullptr};
    VmaAllocator m_allocator = nullptr;
    VulkanDevice* m_Device = nullptr;
    u32 m_Width = 0;
    u32 m_Height = 0;
    u32 m_MipLevels = 1;
    TextureDesc m_Desc;
    SamplerDesc m_SamplerDesc;
    VkDescriptorSet m_ImGuiDescriptorSet = VK_NULL_HANDLE;
    u32 m_TextureIndex = 0;
};
} // namespace UHE::RHI::VULKAN
