#pragma once
#include "RHITypes.h"

namespace UHE::RHI
{

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

    // Whether the image is authored in sRGB and must be linearised by the
    // sampler. This is an IMAGE property, not sampler state: Vulkan encodes it
    // in the image format (eR8G8B8A8Srgb vs eR8G8B8A8Unorm), so it cannot be a
    // descriptor.
    //
    // It lives here rather than in a separate argument because it arrives
    // through the exact same loader path as the filter/wrap state, and because
    // it needs a default that keeps every existing call site unchanged.
    //
    // Default is SRGB because that is what this backend has always used. glTF
    // colour textures (baseColor, emissive) are sRGB; NORMAL, metallicRoughness
    // and occlusion are LINEAR data and gamma-decoding them corrupts the values
    // - a normal map in particular lights the wrong way.
    enum class ColorSpace : u8
    {
        Linear,
        SRGB,
    };

    ColorSpace colorSpace = ColorSpace::SRGB;
};

/**
 * \brief Read/write image resource (render target, sampled texture, storage image).
 *
 * \note The ImGui id is an opaque backend handle (a Vulkan descriptor set) cast to
 *       void* so the editor can hand it to ImGui::Image without knowing the backend.
 */
class RHITexture
{
public:
    virtual ~RHITexture() = default;

    [[nodiscard]] virtual const TextureDesc& GetDesc() const = 0;
    /// \returns the backend handle ImGui needs to draw this texture, or nullptr.
    virtual void* GetImGuiTextureID() = 0;
    /// \returns the bindless slot for this texture, or 0 when bindless is off.
    [[nodiscard]] virtual u32 GetTextureIndex() const { return 0; }
};

} // namespace UHE::RHI
