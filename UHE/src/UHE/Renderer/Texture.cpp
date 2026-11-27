#include "uhepch.h"
#include "Texture.h"
#include "Platform/Vulkan/VulkanTexture2D.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Renderer/Renderer.h"

namespace UHE {

    Ref<Texture2D> Texture2D::Create(const std::string& path, const RHI::SamplerDesc& sampler)
    {
        // Content hands in project-relative paths ("Textures/wall.png"); resolve
        // them against the project/engine mounts before hitting the file system.
        std::filesystem::path resolved = ResolveAssetPath(path);
        // An exported game ships an image cooked to KTX2 beside where the source
        // was (issue #45): "wall.png" is then "wall.png.ktx2", and no PNG is parsed.
        if (std::error_code ec; !std::filesystem::exists(resolved, ec))
        {
            const std::filesystem::path cooked = resolved.string() + ".ktx2";
            if (std::filesystem::exists(cooked, ec))
                resolved = cooked;
        }
        return CreateRef<VulkanTexture2D>(resolved.string(), sampler);
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
