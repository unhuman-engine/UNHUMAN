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
#include "UHE/Renderer/Mipmap.h"

namespace UHE
{

struct KTX2Image
{
    u32 width = 0;
    u32 height = 0;
    // Level 0, RGBA8 raster order - the layout VulkanTexture2D already uploads
    // for stb-decoded images. Kept separate from the chain so a caller that
    // only wants mip 0 never pays for the rest.
    std::vector<u8> rgba;
    // Every level of the file's chain (level 0 included) when the container
    // ships mipmaps; empty when the file is a single level. A file-authored
    // chain uploads directly and the GPU generator stays idle - recomputing a
    // chain the author shipped would both waste time and discard their filter.
    std::vector<MipLevel> mips;
};

// KTX2 container magic. Checking it before handing bytes to the transcoder
// turns "wrong file passed in" into a clean false instead of a parse error
// far away from the caller who chose the wrong source.
bool IsKTX2(const void* data, size_t size);

// Decodes the first face/layer of EVERY mip level the container carries to
// RGBA8. Cubemaps, texture arrays and videos (multi-face/layer) are parsed
// structures the engine cannot upload yet and are rejected loudly; a plain
// 2D texture transcodes fully, chain included.
bool UHE_API LoadKTX2(const void* data, size_t size, KTX2Image& out);

} // namespace UHE
