#pragma once
#include "RHITypes.h"

namespace UHE::RHI {

// Sampler state a loader can request. The backend defaults these to the values it
// has always used, so a texture loaded without a descriptor renders exactly as
// before; glTF's per-texture magFilter/minFilter/wrapS/wrapT map onto this.
struct SamplerDesc
{
    enum class Filter : u8
    {
        Nearest,
        Linear,
        // glTF's 9728 NEAREST_MIPMAP_NEAREST / 9986 NEAREST_MIPMAP_LINEAR.
        NearestMipmapNearest,
        NearestMipmapLinear,
        LinearMipmapNearest,
        LinearMipmapLinear,
    };

    enum class Wrap : u8
    {
        Repeat,
        ClampToEdge,
        MirroredRepeat,
    };

    Filter magFilter = Filter::Linear;
    Filter minFilter = Filter::LinearMipmapLinear;
    Wrap wrapS = Wrap::Repeat;
    Wrap wrapT = Wrap::Repeat;
};
  class RHITexture {
  public:
    virtual ~RHITexture() = default;
    virtual const TextureDesc &GetDesc() const = 0;
    virtual void* GetImGuiTextureID() = 0;
    virtual u32 GetTextureIndex() const { return 0; }
  private:

  };
}