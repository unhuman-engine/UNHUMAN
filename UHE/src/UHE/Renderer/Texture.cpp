#include "uhepch.h"
#include "Texture.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "Platform/Vulkan/VulkanTexture2D.h"

namespace UHE {

    Ref<Texture2D> Texture2D::Create(const std::string& path, const RHI::SamplerDesc& sampler)
    {
        // Content hands in project-relative paths ("Textures/wall.png"); resolve
        // them against the project/engine mounts before hitting the file system.
        return CreateRef<VulkanTexture2D>(ResolveAssetPath(path).string(), sampler);
    }

    Ref<Texture2D> Texture2D::Create(u32 width, u32 height, const RHI::SamplerDesc& sampler)
    {
        return CreateRef<VulkanTexture2D>(width, height, sampler);
    }

    Ref<Texture2D> Texture2D::CreateFromMemory(const void* data, size_t size, const RHI::SamplerDesc& sampler)
    {
        return CreateRef<VulkanTexture2D>(data, size, sampler);
    }

}
