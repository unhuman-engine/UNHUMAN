#include "uhepch.h"
#include "VulkanTexture2D.h"
#include <fstream>
#include <stb_image.h>
#include "Platform/Vulkan/VulkanDevice.h"
#include "UHE/Renderer/KTX2.h"
#include "UHE/Renderer/Renderer.h"
#include "VulkanTexture.h"

namespace UHE
{

namespace
{
// Shared upload for decoded pixels. Both entry points (file, memory)
// funnel here once the bytes are RGBA8, so the KTX2 path and the stb
// path cannot drift apart in sampler/format handling. A file-authored
// mip chain uploads level by level and the GPU generator stays idle
// (recomputing would discard the author's filter); an empty chain is
// the plain mip-0 upload with GPU generation.
void UploadRGBA8(Ref<RHI::VULKAN::VulkanTexture>& target, const void* pixels, u32 width, u32 height,
                 const RHI::SamplerDesc& sampler, std::span<const UHE::MipLevel> mipChain = {})
{
    target = CreateRef<RHI::VULKAN::VulkanTexture>();
    // Must precede CreateTexture: the sampler is built inside it.
    target->SetSamplerDesc(sampler);
    auto& rhiDevice = Renderer::GetDevice();
    auto* vulkanDevice = static_cast<RHI::VULKAN::VulkanDevice*>(&rhiDevice);

    const size_t dataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (mipChain.empty())
        target->CreateTexture(*vulkanDevice, pixels, width, height, dataSize);
    else
        target->CreateTextureWithMips(*vulkanDevice, pixels, width, height, dataSize, mipChain);
}

void UploadWhiteFallback(Ref<RHI::VULKAN::VulkanTexture>& target)
{
    target = CreateRef<RHI::VULKAN::VulkanTexture>();
    auto& rhiDevice = Renderer::GetDevice();
    auto* vulkanDevice = static_cast<RHI::VULKAN::VulkanDevice*>(&rhiDevice);
    u32 whiteData = 0xffffffff;
    target->CreateTexture(*vulkanDevice, &whiteData, 1, 1, 4);
}
} // namespace

VulkanTexture2D::VulkanTexture2D(const std::string& path, const RHI::SamplerDesc& sampler)
{
    // External .ktx2 references (a .gltf naming "tex.ktx2" beside it) take
    // the same route as embedded ones; the GLB path reaches the memory
    // constructor below instead.
    if (path.size() >= 5 &&
        (path.compare(path.size() - 5, 5, ".ktx2") == 0 || path.compare(path.size() - 5, 5, ".KTX2") == 0))
    {
        std::ifstream file(path, std::ios::binary);
        std::vector<char> bytes;
        if (file)
        {
            file.seekg(0, std::ios::end);
            bytes.resize(static_cast<size_t>(file.tellg()));
            file.seekg(0, std::ios::beg);
            file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        KTX2Image image;
        if (LoadKTX2(bytes.data(), bytes.size(), image))
        {
            m_Width = image.width;
            m_Height = image.height;
            UploadRGBA8(m_VulkanTexture, image.rgba.data(), m_Width, m_Height, sampler, image.mips);
            return;
        }

        UHE_CORE_ERROR("KTX2 texture decode failed for {0}; falling back to white", path);
        m_Width = 1;
        m_Height = 1;
        UploadWhiteFallback(m_VulkanTexture);
        return;
    }

    int width, height, channels;
    stbi_set_flip_vertically_on_load(1);
    stbi_uc* data = stbi_load(path.c_str(), &width, &height, &channels, 4);

    if (data)
    {
        m_Width = width;
        m_Height = height;
        UploadRGBA8(m_VulkanTexture, data, m_Width, m_Height, sampler);
        stbi_image_free(data);
    }
    else
    {
        UHE_CORE_ERROR("Failed to load image: {0}", path);

        // Fallback to a 1x1 white texture to prevent crashes
        m_Width = 1;
        m_Height = 1;
        UploadWhiteFallback(m_VulkanTexture);
    }
}

VulkanTexture2D::VulkanTexture2D(const void* inData, size_t size, const RHI::SamplerDesc& sampler)
{
    // KTX2/Basis files do not go through stb. GLB containers embed the
    // whole .ktx2 verbatim, so the magic check - not the file extension -
    // is what routes it; a renamer cannot break the decode, and a PNG
    // handed to this constructor is unaffected.
    if (IsKTX2(inData, size))
    {
        KTX2Image image;
        if (LoadKTX2(inData, size, image))
        {
            m_Width = image.width;
            m_Height = image.height;
            UploadRGBA8(m_VulkanTexture, image.rgba.data(), m_Width, m_Height, sampler, image.mips);
            return;
        }

        UHE_CORE_ERROR("KTX2 texture decode failed; falling back to white");

        m_Width = 1;
        m_Height = 1;
        UploadWhiteFallback(m_VulkanTexture);
        return;
    }

    int width, height, channels;
    stbi_set_flip_vertically_on_load(1);
    stbi_uc* data = stbi_load_from_memory(static_cast<const stbi_uc*>(inData), size, &width, &height, &channels, 4);

    if (data)
    {
        m_Width = width;
        m_Height = height;
        UploadRGBA8(m_VulkanTexture, data, m_Width, m_Height, sampler);
        stbi_image_free(data);
    }
    else
    {
        UHE_CORE_ERROR("Failed to load image from memory!");

        // Fallback to a 1x1 white texture to prevent crashes
        m_Width = 1;
        m_Height = 1;
        UploadWhiteFallback(m_VulkanTexture);
    }
}

VulkanTexture2D::VulkanTexture2D(u32 width, u32 height, const RHI::SamplerDesc& sampler)
    : m_Width(width), m_Height(height)
{
    m_VulkanTexture = CreateRef<RHI::VULKAN::VulkanTexture>();
    // Must precede CreateTexture: the sampler is built inside it.
    m_VulkanTexture->SetSamplerDesc(sampler);
    auto& rhiDevice = Renderer::GetDevice();
    auto* vulkanDevice = static_cast<RHI::VULKAN::VulkanDevice*>(&rhiDevice);

    size_t dataSize = static_cast<size_t>(m_Width) * static_cast<size_t>(m_Height) * 4;

    // For blank textures, allocate white pixel data
    u32* whiteData = new u32[static_cast<size_t>(m_Width) * static_cast<size_t>(m_Height)];
    for (u32 i = 0; i < static_cast<size_t>(m_Width) * static_cast<size_t>(m_Height); i++)
        whiteData[i] = 0xffffffff;

    m_VulkanTexture->CreateTexture(*vulkanDevice, whiteData, m_Width, m_Height, dataSize);
    delete[] whiteData;
}

VulkanTexture2D::~VulkanTexture2D()
{
    // VulkanTexture destructor or cleanup will handle resources
}

void* VulkanTexture2D::GetImGuiTextureID()
{
    if (m_VulkanTexture)
        return m_VulkanTexture->GetImGuiTextureID();
    return nullptr;
}

} // namespace UHE
