#include "TextureCook.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include "basisu_comp.h"

// A private copy of stb_image: the encoder library must not depend on the one
// libUHE exports, and static linkage keeps the two from clashing in the editor.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_PSD
#define STBI_NO_GIF
#include "stb_image.h"

namespace UHE::Cook
{
namespace
{
constexpr std::uint8_t kKTX2Magic[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

bool IsKTX2(std::span<const std::uint8_t> bytes)
{
    return bytes.size() >= sizeof(kKTX2Magic) && std::memcmp(bytes.data(), kKTX2Magic, sizeof(kKTX2Magic)) == 0;
}

float ToLinear(std::uint8_t v)
{
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::uint8_t ToSrgb(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(s * 255.0f));
}

/// Halves an RGBA8 image with a 2x2 box filter (in linear light for colour).
void Halve(std::vector<std::uint8_t>& rgba, std::uint32_t& width, std::uint32_t& height, bool srgb)
{
    const std::uint32_t w = std::max(width / 2, 1u);
    const std::uint32_t h = std::max(height / 2, 1u);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            for (std::uint32_t c = 0; c < 4; ++c)
            {
                const bool colour = srgb && c < 3;
                float sum = 0.0f;
                for (std::uint32_t dy = 0; dy < 2; ++dy)
                    for (std::uint32_t dx = 0; dx < 2; ++dx)
                    {
                        const std::uint32_t sx = std::min(x * 2 + dx, width - 1);
                        const std::uint32_t sy = std::min(y * 2 + dy, height - 1);
                        const std::uint8_t v = rgba[(static_cast<std::size_t>(sy) * width + sx) * 4 + c];
                        sum += colour ? ToLinear(v) : static_cast<float>(v);
                    }
                sum *= 0.25f;
                out[(static_cast<std::size_t>(y) * w + x) * 4 + c] =
                    colour ? ToSrgb(sum) : static_cast<std::uint8_t>(std::lround(sum));
            }
    rgba = std::move(out);
    width = w;
    height = h;
}

/// Bump when the encoder settings below change: older cache entries are then never hit.
constexpr std::uint64_t kCacheVersion = 1;

std::uint64_t Fnv(const void* data, std::size_t size, std::uint64_t hash = 1469598103934665603ull)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

std::filesystem::path CachePath(std::span<const std::uint8_t> encoded, TextureRole role, bool uastc,
                                const TextureCookOptions& options)
{
    const std::uint64_t settings[] = {kCacheVersion, static_cast<std::uint64_t>(role), uastc ? 1u : 0u, options.MaxSize,
                                      static_cast<std::uint64_t>(options.Etc1sQuality)};
    std::uint64_t hash = Fnv(encoded.data(), encoded.size());
    hash = Fnv(settings, sizeof(settings), hash);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.ktx2", static_cast<unsigned long long>(hash));
    return options.CacheDir / name;
}

bool ReadFile(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return IsKTX2(out);
}

void WriteCache(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            return;
    }
    std::filesystem::rename(temporary, path, ec); // a cache that cannot be written only costs time
}

void InitEncoderOnce()
{
    static std::once_flag once;
    std::call_once(once, [] { basisu::basisu_encoder_init(); });
}
} // namespace

bool ParseTextureCodec(const std::string& name, TextureCodec& out)
{
    if (name == "keep")
        out = TextureCodec::Keep;
    else if (name == "auto")
        out = TextureCodec::Auto;
    else if (name == "etc1s")
        out = TextureCodec::ETC1S;
    else if (name == "uastc")
        out = TextureCodec::UASTC;
    else
        return false;
    return true;
}

bool TextureOptionsForPlatform(const std::string& platform, TextureCookOptions& out)
{
    out = TextureCookOptions{};
    if (platform == "desktop")
        return true;
    if (platform == "mobile" || platform == "android")
    {
        out.MaxSize = 2048;
        return true;
    }
    return false;
}

bool EncodeKTX2(std::span<const std::uint8_t> encoded, TextureRole role, const TextureCookOptions& options,
                std::vector<std::uint8_t>& out, std::string& error)
{
    if (IsKTX2(encoded) || options.Codec == TextureCodec::Keep)
    {
        out.assign(encoded.begin(), encoded.end());
        return true;
    }

    const bool uastc =
        options.Codec == TextureCodec::UASTC || (options.Codec == TextureCodec::Auto && role != TextureRole::Color);
    std::filesystem::path cached;
    if (!options.CacheDir.empty())
    {
        cached = CachePath(encoded, role, uastc, options);
        if (ReadFile(cached, out))
            return true;
    }

    // The runtime's stb load flips rows (VulkanTexture2D); a KTX2 is uploaded
    // as stored, so the flip happens here, once, before encoding.
    stbi_set_flip_vertically_on_load_thread(1);
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels =
        stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &channels, STBI_rgb_alpha);
    if (pixels == nullptr)
    {
        error = std::string("the image does not decode: ") + stbi_failure_reason();
        return false;
    }
    std::uint32_t width = static_cast<std::uint32_t>(w);
    std::uint32_t height = static_cast<std::uint32_t>(h);
    std::vector<std::uint8_t> rgba(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);

    const bool srgb = role == TextureRole::Color;
    if (options.MaxSize > 0)
        while (std::max(width, height) > options.MaxSize)
            Halve(rgba, width, height, srgb);

    InitEncoderOnce();
    const std::uint32_t threads =
        options.Threads != 0 ? options.Threads : std::max(1u, std::thread::hardware_concurrency());
    basisu::job_pool jobs(threads);
    basisu::basis_compressor_params params;
    params.m_pJob_pool = &jobs;
    params.m_multithreading = threads > 1;
    params.m_status_output = false;

    basisu::image source(width, height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
            source(x, y).set(p[0], p[1], p[2], p[3]);
        }
    params.m_source_images.push_back(source);
    params.set_format_mode(uastc ? basist::basis_tex_format::cUASTC_LDR_4x4 : basist::basis_tex_format::cETC1S);
    params.set_srgb_options(srgb);
    params.m_create_ktx2_file = true;
    params.m_mip_gen = true;
    // Normal maps keep unit vectors in every level, which averaging alone does not.
    params.m_mip_renormalize = role == TextureRole::Normal;
    // Settings measured on ForestLake's 2K maps, 4 threads: UASTC "faster"
    // without RDO is 8 s at 36 dB (RDO took 87 s and saved nothing after
    // zstd); ETC1S at quality 128, compression level 1, 13 s at 30 dB.
    if (uastc)
    {
        params.m_pack_uastc_ldr_4x4_flags = basisu::cPackUASTCLevelFaster;
        params.m_ktx2_uastc_supercompression = basist::KTX2_SS_ZSTANDARD;
    }
    else
    {
        params.m_quality_level = std::clamp(options.Etc1sQuality, 1, 255);
        params.m_etc1s_compression_level = 1;
    }

    basisu::basis_compressor compressor;
    if (!compressor.init(params))
    {
        error = "the Basis encoder refused its parameters";
        return false;
    }
    const basisu::basis_compressor::error_code result = compressor.process();
    if (result != basisu::basis_compressor::cECSuccess)
    {
        error = "the Basis encoder failed (code " + std::to_string(static_cast<int>(result)) + ")";
        return false;
    }
    const basisu::uint8_vec& ktx2 = compressor.get_output_ktx2_file();
    out.assign(ktx2.begin(), ktx2.end());
    if (!cached.empty())
        WriteCache(cached, out);
    return true;
}

} // namespace UHE::Cook
