// CPU-side mipmap generation (issue #42: "Mipmap generation (CPU + GPU)").
//
// The GPU path (VulkanTexture::GenerateMipmaps) blits the chain down on
// device; this module exists because not everything has a device:
//   - the headless test harness (a broken downsampler must fail the test
//     run, not a scene),
//   - editor thumbnails, which need a small copy of a texture that is never
//     uploaded,
//   - any future offline/export path.
//
// Deliberately free of RHI/Vulkan types.
//
// Filtering: a plain 2x2 box filter over the sRGB-ENCODED bytes. Averaging in
// encoded space darkens slightly relative to a linear-space filter; that is
// the standard trade for a cheap CPU path and matches what most offline
// mipmappers did for decades. The GPU chain generation has the same property.

#pragma once

#include <span>
#include <vector>

#include "UHE/Core/Core.h"

namespace UHE
{

// One RGBA8 image level: width x height x 4 bytes, raster order.
struct MipLevel
{
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba;
};

// Half-size copy of an RGBA8 image (2x2 box filter). Odd dimensions round up,
// matching how the GPU blit path and the KTX2 container size their levels.
[[nodiscard]] MipLevel UHE_API DownsampleRGBA8(std::span<const u8> src, u32 width, u32 height);

// Full chain: level 0 is a copy of the source, then repeated halving until
// 1x1. A 1x1 source yields exactly one level.
[[nodiscard]] std::vector<MipLevel> UHE_API GenerateMipmapChainRGBA8(std::span<const u8> src, u32 width,
                                                                     u32 height);

// Byte size of an RGBA8 image of the given dimensions.
[[nodiscard]] constexpr size_t RGBA8Size(const u32 width, const u32 height)
{
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
}

} // namespace UHE
