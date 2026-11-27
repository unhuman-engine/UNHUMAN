#pragma once

/**
 * @file TextureCook.h
 * @brief Encodes the images a cooked model carries to KTX2 / Basis Universal (issue #45).
 *
 * The runtime decodes a KTX2 image wherever it would decode a PNG or JPEG
 * (VulkanTexture2D routes by magic), so a cook that stores KTX2 needs no
 * runtime change, and the game no longer parses PNG or JPEG for its models.
 *
 * Deliberately outside libUHE: the basisu encoder is large and only the
 * editor's Export and the cook tools (uhe_export, uhe_cook) link it; the
 * shipped runtime links only the transcoder.
 */

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace UHE::Cook
{

/// @brief Which Basis format an image is encoded to.
enum class TextureCodec : std::uint8_t
{
    Keep,  ///< store the source bytes unchanged (PNG/JPEG stay PNG/JPEG)
    Auto,  ///< ETC1S for colour images, UASTC for normal maps and other data
    ETC1S, ///< smallest (about 1-2 bits per pixel, ~13 s for 2K); too lossy for normal maps
    UASTC, ///< high quality (zstd, about 8 bits per pixel, ~8 s for 2K)
};

/// @brief What an image holds, which decides the codec under Auto and the colour handling.
enum class TextureRole : std::uint8_t
{
    Color,  ///< sRGB colour: base colour, emissive, sheen colour, ...
    Data,   ///< linear data: metallic-roughness, occlusion, masks
    Normal, ///< tangent-space normal map
};

struct TextureCookOptions
{
    TextureCodec Codec = TextureCodec::Auto;
    /// Largest width or height kept; a bigger image is halved until it fits. 0: no limit.
    std::uint32_t MaxSize = 0;
    /// ETC1S quality, 1-255.
    int Etc1sQuality = 128;
    /// Worker threads for one image; 0 picks the hardware's count.
    std::uint32_t Threads = 0;
    /**
     * Where encoded images are kept between cooks, keyed by the source bytes
     * and these settings; empty: no cache. An export re-encodes only images
     * that changed (a 2K image takes seconds, a project's worth minutes).
     */
    std::filesystem::path CacheDir;
};

/// @brief Parses a codec name (keep, auto, etc1s, uastc); false for anything else.
bool ParseTextureCodec(const std::string& name, TextureCodec& out);

/**
 * @brief The texture settings a platform's export uses (the cook's platform variants).
 *
 * `desktop`: Auto codecs at full size. `mobile` (also `android`): Auto codecs,
 * images at most 2048 pixels on a side, which is what a phone's memory budget
 * and its texture units are comfortable with. Basis files transcode on the
 * device itself, so the platforms differ in size and codec choice, not format.
 *
 * @return false for an unknown platform name.
 */
bool TextureOptionsForPlatform(const std::string& platform, TextureCookOptions& out);

/**
 * @brief Encodes one PNG/JPEG/BMP/TGA image to a KTX2 file with a full mip chain.
 *
 * The raster is flipped vertically first, exactly as the runtime's stb load
 * flips a PNG, so the cooked image samples the same as its source. A source
 * that already is KTX2 is returned unchanged.
 *
 * @return false (with \p error) when the image does not decode or encode.
 */
bool EncodeKTX2(std::span<const std::uint8_t> encoded, TextureRole role, const TextureCookOptions& options,
                std::vector<std::uint8_t>& out, std::string& error);

} // namespace UHE::Cook
