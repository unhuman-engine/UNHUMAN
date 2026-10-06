#include "uhepch.h"
#include "KTX2.h"
#include "basisu_transcoder.h"

namespace UHE
{

namespace
{

// KTX2 identifier, byte-for-byte as the transcoder itself defines it.
constexpr unsigned char kKTX2Magic[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

// basisu's transcoder lookup tables must be initialised once per process.
struct TranscoderInit
{
    TranscoderInit() { basist::basisu_transcoder_init(); }
};

const TranscoderInit g_transcoderInit;

} // namespace

bool IsKTX2(const void* data, size_t size)
{
    return data != nullptr && size >= sizeof(kKTX2Magic) && std::memcmp(data, kKTX2Magic, sizeof(kKTX2Magic)) == 0;
}

bool LoadKTX2(const void* data, size_t size, KTX2Image& out)
{
    if (!IsKTX2(data, size))
    {
        UHE_CORE_ERROR("KTX2 load failed: data is not a KTX2 container");
        return false;
    }

    basist::ktx2_transcoder transcoder;
    if (!transcoder.init(data, static_cast<u32>(size)))
    {
        UHE_CORE_ERROR("KTX2 load failed: header could not be parsed");
        return false;
    }

    // HDR formats cannot be transcoded to LDR RGBA8 without tone mapping the
    // engine does not have; refuse instead of producing a black texture.
    if (!transcoder.is_ldr())
    {
        UHE_CORE_ERROR("KTX2 load failed: HDR ({}), only LDR content is supported",
                       transcoder.is_uastc() ? "uastc-hdr" : "astc-hdr");
        return false;
    }

    if (transcoder.get_faces() != 1)
    {
        UHE_CORE_ERROR("KTX2 load failed: {} faces, cubemaps are not supported yet", transcoder.get_faces());
        return false;
    }

    if (transcoder.get_layers() > 1)
    {
        UHE_CORE_ERROR("KTX2 load failed: {} layers, texture arrays/videos are not supported yet",
                       transcoder.get_layers());
        return false;
    }

    out.width = transcoder.get_width();
    out.height = transcoder.get_height();
    if (out.width == 0 || out.height == 0)
    {
        UHE_CORE_ERROR("KTX2 load failed: zero-sized image ({}x{})", out.width, out.height);
        return false;
    }

    if (!transcoder.start_transcoding())
    {
        UHE_CORE_ERROR("KTX2 load failed: start_transcoding returned false");
        return false;
    }

    out.rgba.resize(RGBA8Size(out.width, out.height));

    // cTFRGBA32 is raster-order RGBA8, byte order R,G,B,A - the exact layout
    // the upload path expects. Every level is transcoded ONCE into the chain;
    // out.rgba then aliases level 0 for callers that never look further.
    out.mips.reserve(transcoder.get_levels());
    for (u32 level = 0; level < transcoder.get_levels(); ++level)
    {
        basist::ktx2_image_level_info info{};
        if (!transcoder.get_image_level_info(info, level, 0, 0))
        {
            UHE_CORE_ERROR("KTX2 load failed: level {} info unavailable", level);
            return false;
        }

        MipLevel mip;
        mip.width = info.m_orig_width;
        mip.height = info.m_orig_height;
        mip.rgba.resize(RGBA8Size(mip.width, mip.height));

        if (!transcoder.transcode_image_level(level, 0, 0, mip.rgba.data(), static_cast<u32>(mip.width) * mip.height,
                                              basist::transcoder_texture_format::cTFRGBA32))
        {
            UHE_CORE_ERROR("KTX2 load failed: level {} transcode returned false ({}x{})", level, mip.width, mip.height);
            return false;
        }
        out.mips.push_back(std::move(mip));
    }

    out.rgba = out.mips.front().rgba;
    return true;
}

} // namespace UHE
