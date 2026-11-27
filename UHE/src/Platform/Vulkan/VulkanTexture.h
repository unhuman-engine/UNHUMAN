#pragma once
#include <cstddef>
#include <span>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHICore.h"
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
    /// @brief Wraps an image the RHI does not own (RHIDevice::ImportTexture); a view is made when none is given.
    void InitImported(VulkanDevice& device, vk::Image image, vk::ImageView view, const TextureDesc& desc);
    /// @brief A view over part of \p parent (RHIDevice::CreateTextureView); the parent must outlive it.
    void InitView(VulkanDevice& device, VulkanTexture& parent, const TextureViewDesc& desc);
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

    /// @brief The subresources this texture covers, for barriers and copies (a view covers part of its image).
    [[nodiscard]] vk::ImageSubresourceRange GetRange(u32 baseMip, u32 mipCount, u32 baseLayer, u32 layerCount) const;
    [[nodiscard]] vk::ImageAspectFlags GetAspect() const { return m_Aspect; }
    [[nodiscard]] u32 GetMipLevels() const { return m_MipLevels; }
    [[nodiscard]] u32 GetWidth() const { return m_Width; }
    [[nodiscard]] u32 GetHeight() const { return m_Height; }
    /// @brief The first mip and layer of the image this texture starts at (0 unless it is a view).
    [[nodiscard]] u32 GetBaseMip() const { return m_BaseMip; }
    [[nodiscard]] u32 GetBaseLayer() const { return m_BaseLayer; }
    [[nodiscard]] u32 GetLayerCount() const { return m_LayerCount; }

private:
    vk::Image textureImage{nullptr};
    VmaAllocation textureImageMemory = nullptr;
    vk::raii::ImageView textureImageView{nullptr};
    /// Depth aspect only, for sampling a depth texture; null for colour formats.
    vk::raii::ImageView m_SampledDepthView{nullptr};
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
    vk::ImageAspectFlags m_Aspect = vk::ImageAspectFlagBits::eColor;
    u32 m_BaseMip = 0;
    u32 m_BaseLayer = 0;
    u32 m_LayerCount = 1;
};
} // namespace UHE::RHI::VULKAN
