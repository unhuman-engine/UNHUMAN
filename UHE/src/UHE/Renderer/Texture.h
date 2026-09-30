#pragma once
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHITexture.h"
#include <string>

namespace UHE {

class UHE_API Texture {
public:
    virtual ~Texture() = default;

    virtual u32 GetWidth() const = 0;
    virtual u32 GetHeight() const = 0;

    virtual void Bind(u32 slot = 0) const = 0;
    virtual void* GetImGuiTextureID() = 0;
    virtual RHI::TextureHandle GetTextureHandle() const = 0;
    virtual u32 GetTextureIndex() const { return 0; }

    virtual bool operator==(const Texture& other) const = 0;
};

class UHE_API Texture2D : public Texture {
public:
    virtual ~Texture2D() = default;

    // `sampler` is optional and defaults to the backend's historical state
    // (linear magnification, trilinear minification, repeat). glTF declares
    // magFilter/minFilter/wrapS/wrapT per texture and the loader passes them
    // through here; a texture loaded without one renders exactly as before.
    static Ref<Texture2D> Create(const std::string& path, const RHI::SamplerDesc& sampler = {});
    static Ref<Texture2D> Create(u32 width, u32 height, const RHI::SamplerDesc& sampler = {});
    static Ref<Texture2D> CreateFromMemory(const void* data, size_t size, const RHI::SamplerDesc& sampler = {});
};

} // namespace UHE
