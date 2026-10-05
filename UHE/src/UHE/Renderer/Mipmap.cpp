#include "uhepch.h"
#include "Mipmap.h"

#include <algorithm>

namespace UHE
{

namespace
{

// Byte offset of texel (x, y) in a raster-order RGBA8 image of the given width.
constexpr size_t PixelOffset(const u32 x, const u32 y, const u32 width)
{
    return (static_cast<size_t>(y) * width + x) * 4;
}

} // namespace

MipLevel DownsampleRGBA8(const std::span<const u8> src, const u32 width, const u32 height)
{
    const u32 dstWidth = std::max(width / 2, 1u);
    const u32 dstHeight = std::max(height / 2, 1u);

    MipLevel out;
    out.width = dstWidth;
    out.height = dstHeight;
    out.rgba.resize(RGBA8Size(dstWidth, dstHeight));

    for (u32 y = 0; y < dstHeight; ++y)
    {
        for (u32 x = 0; x < dstWidth; ++x)
        {
            // Each destination texel averages the 2x2 source block at (2x, 2y).
            // Odd source edges: the last row/column duplicates its edge texel,
            // so a 5-wide image downsamples like a 6-wide one with the final
            // column repeated - the same convention the block-based formats
            // and the GPU blit path land on.
            const u32 sx0 = std::min(x * 2, width - 1);
            const u32 sy0 = std::min(y * 2, height - 1);
            const u32 sx1 = std::min(x * 2 + 1, width - 1);
            const u32 sy1 = std::min(y * 2 + 1, height - 1);

            for (u32 c = 0; c < 4; ++c)
            {
                const u32 sum = static_cast<u32>(src[PixelOffset(sx0, sy0, width) + c]) +
                                static_cast<u32>(src[PixelOffset(sx1, sy0, width) + c]) +
                                static_cast<u32>(src[PixelOffset(sx0, sy1, width) + c]) +
                                static_cast<u32>(src[PixelOffset(sx1, sy1, width) + c]);
                out.rgba[PixelOffset(x, y, dstWidth) + c] = static_cast<u8>((sum + 2) / 4);
            }
        }
    }

    return out;
}

std::vector<MipLevel> GenerateMipmapChainRGBA8(const std::span<const u8> src, const u32 width, const u32 height)
{
    std::vector<MipLevel> chain;
    if (src.empty() || width == 0 || height == 0)
        return chain;

    MipLevel level0;
    level0.width = width;
    level0.height = height;
    level0.rgba.assign(src.begin(), src.end());
    chain.push_back(std::move(level0));

    u32 w = width;
    u32 h = height;
    while (w > 1 || h > 1)
    {
        MipLevel next = DownsampleRGBA8(chain.back().rgba, w, h);
        w = next.width;
        h = next.height;
        chain.push_back(std::move(next));
    }

    return chain;
}

} // namespace UHE
