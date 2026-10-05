// KTX2 / Basis Universal decoding (issue #42, and issue #29 Tier 4).
//
// Deliberately free of RHI/Vulkan types: the decode runs on the CPU before
// anything is uploaded, which keeps it linkable into the headless test
// harness where a real encode -> decode round trip is asserted with the
// vendored basis_universal encoder.
//
// Scope note: this transcodes to UNCOMPRESSED RGBA8. Block-compressed output
// (BC7/ASTC at runtime) needs device format-support queries and a multi-mip
// upload path in the RHI first; until then the GPU mipmap generator in
// VulkanTexture produces the chain from mip 0 exactly as it does for PNGs.

#pragma once

#include <cstddef>
#include <vector>

#include "UHE/Core/Core.h"

namespace UHE
{

struct KTX2Image
{
    u32 width = 0;
    u32 height = 0;
    // 4 bytes per pixel, RGBA8, raster order - the layout VulkanTexture2D
    // already uploads for stb-decoded images.
    std::vector<u8> rgba;
};

// KTX2 container magic. Checking it before handing bytes to the transcoder
// turns "wrong file passed in" into a clean false instead of a parse error
// far away from the caller who chose the wrong source.
bool IsKTX2(const void* data, size_t size);

// Decodes the FIRST face of mip level 0 to RGBA8. Arrays, videos (layers),
// cubemaps and mip chains are parsed structures the engine cannot upload yet;
// transcoding mip 0 / face 0 is what matches the single-2D-texture path every
// other image format takes here.
bool UHE_API LoadKTX2(const void* data, size_t size, KTX2Image& out);

} // namespace UHE
