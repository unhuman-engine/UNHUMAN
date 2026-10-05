#pragma once
#include <cstddef>
#include <span>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHITexture.h"

namespace UHE::RHI::VULKAN
{
class VulkanDevice;
class VulkanLogicalDevice;

/**
 * \brief Vulkan texture: image + view + sampler, plus its ImGui and bindless handles.
 *
 * Covers the full lifecycle of a GPU image — allocation, staging upload, mip
 * generation and descriptor registration.
 *
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkImage.html
 */
class VulkanTexture final : public RHITexture
{
public:
    VulkanTexture() = default;
    ~VulkanTexture() override;

    [[nodiscard]] const TextureDesc& GetDesc() const override { return m_Desc; }
    void* GetImGuiTextureID() override;
    [[nodiscard]] u32 GetTextureIndex() const override { return m_TextureIndex; }

    void Init(VulkanDevice& device, const TextureDesc& desc);
    void CreateImage(VulkanLogicalDevice& device, uint32_t width, uint32_t height, uint32_t mipLevels,
                     vk::Format format, vk::ImageUsageFlags usage, VmaMemoryUsage memUsage, vk::ImageTiling tiling,
                     vk::Image& image, VmaAllocation& imageMemory);
    void CreateTexture(VulkanDevice& device, const void* pixelData, u32 width, u32 height, size_t dataSize);
    void ExecuteCopyCommand(VulkanDevice& device, VkBuffer srcBuffer, vk::Image dstImage, uint32_t width,
                            uint32_t height, uint32_t mipLevels);
    void GenerateMipmaps(VulkanDevice& device, vk::Image image, vk::Format imageFormat, int32_t texWidth,
                         int32_t texHeight, uint32_t mipLevels);
    void UpdateTexture(std::span<const u8> data);

    [[nodiscard]] vk::Image& GetImage() { return textureImage; }
    [[nodiscard]] vk::raii::ImageView& GetImageView() { return textureImageView; }
    [[nodiscard]] vk::raii::Sampler& GetSampler() { return textureSampler; }

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
    VkDescriptorSet m_ImGuiDescriptorSet = VK_NULL_HANDLE;
    u32 m_TextureIndex = 0;
};
} // namespace UHE::RHI::VULKAN
