#pragma once
#include <string>
#include "Platform/Vulkan/VulkanTexture.h"
#include "UHE/Renderer/Texture.h"

namespace UHE
{
class VulkanTexture2D final : public Texture2D
{
public:
    VulkanTexture2D(const std::string& path, const RHI::SamplerDesc& sampler = {});
    VulkanTexture2D(u32 width, u32 height, const RHI::SamplerDesc& sampler = {});
    VulkanTexture2D(const void* data, size_t size, const RHI::SamplerDesc& sampler = {});
    ~VulkanTexture2D() override;

    u32 GetWidth() const override { return m_Width; }
    u32 GetHeight() const override { return m_Height; }

    void Bind(u32 slot = 0) const override {}
    void* GetImGuiTextureID() override;
    RHI::TextureHandle GetTextureHandle() const override
    {
        return reinterpret_cast<RHI::TextureHandle>(m_VulkanTexture.get());
    }
    u32 GetTextureIndex() const override { return m_VulkanTexture ? m_VulkanTexture->GetTextureIndex() : 0; }

    bool operator==(const Texture& other) const override { return this == &other; }

private:
    u32 m_Width = 0;
    u32 m_Height = 0;
    Ref<RHI::VULKAN::VulkanTexture> m_VulkanTexture;
};
} // namespace UHE
