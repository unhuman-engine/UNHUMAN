#pragma once
#include <cstddef>
#include <span>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHITexture.h"
#include "UHE/Renderer/Mipmap.h"

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

    // Same, but with an already-computed mip chain (levels 1..N; level 0 is
    // `pixelData`). A KTX2 container ships its chain, so uploading it directly
    // both skips the GPU generation pass and preserves the author's filter -
    // recomputing would throw their mips away. When the chain is empty this
    // behaves exactly like CreateTexture and the GPU builds the levels.
    void CreateTextureWithMips(VulkanDevice& device, const void* pixelData, u32 width, u32 height, size_t dataSize,
                               std::span<const UHE::MipLevel> mipChain);

    // Sampler state for the next CreateTexture call. The backend default matches
    // what it always used (linear/linear, repeat on all axes), so a texture
    // loaded without calling this renders unchanged.
    void SetSamplerDesc(const SamplerDesc& desc) { m_SamplerDesc = desc; }
    void ExecuteCopyCommand(VulkanDevice& device, VkBuffer srcBuffer, vk::Image dstImage, uint32_t width,
                            uint32_t height, uint32_t mipLevels, std::span<const u32> levelByteSizes = {});
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
    SamplerDesc m_SamplerDesc;
    VkDescriptorSet m_ImGuiDescriptorSet = VK_NULL_HANDLE;
    u32 m_TextureIndex = 0;
};
} // namespace UHE::RHI::VULKAN
