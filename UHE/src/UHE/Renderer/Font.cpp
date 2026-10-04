#include "uhepch.h"
#include "Font.h"
#include "UHE/AssestsManager/VfsSystem.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/Renderer.h"
#include "msdf-atlas-gen/FontGeometry.h"
#include "msdf-atlas-gen/GlyphGeometry.h"
#include "msdf-atlas-gen/msdf-atlas-gen.h"
#include "msdfgen-ext.h"
#include "msdfgen.h"

namespace UHE
{

struct Font2D::Impl
{
    std::vector<msdf_atlas::GlyphGeometry> Glyphs;
    msdf_atlas::FontGeometry FontGeometry;

    Impl() : FontGeometry(&Glyphs) {}
};

template <typename T, typename S, int N, msdf_atlas::GeneratorFunction<S, N> GenFunc>
static RHI::TextureHandle CreateAtlasTexture(const std::vector<msdf_atlas::GlyphGeometry>& glyphs, u32 width,
                                             u32 height)
{
    msdf_atlas::GeneratorAttributes attributes;
    attributes.config.overlapSupport = true;
    attributes.scanlinePass = true;

    msdf_atlas::ImmediateAtlasGenerator<S, N, GenFunc, msdf_atlas::BitmapAtlasStorage<T, N>> generator(width, height);
    generator.setAttributes(attributes);
    // Force single-threaded generation to align with the rest of the engine.
    // This avoids spawning uncontrolled threads until a proper Job System is added.
    generator.setThreadCount(1);
    generator.generate(glyphs.data(), static_cast<i32>(glyphs.size()));

    msdfgen::BitmapConstSection<T, N> bitmap = static_cast<msdfgen::BitmapConstSection<T, N>>(generator.atlasStorage());

    bool allZeros = true;
    for (size_t i = 0; i < width * height * N; i++)
    {
        if (reinterpret_cast<const u8*>(bitmap.pixels)[i] != 0)
        {
            allZeros = false;
            break;
        }
    }
    if (allZeros)
    {
        UHE_CORE_ERROR("MSDF ATLAS IS COMPLETELY BLACK!!!");
    }
    else
    {
        UHE_CORE_INFO("MSDF Atlas generated successfully. First few bytes: {}, {}, {}, {}",
                      reinterpret_cast<const u8*>(bitmap.pixels)[0], reinterpret_cast<const u8*>(bitmap.pixels)[1],
                      reinterpret_cast<const u8*>(bitmap.pixels)[2], reinterpret_cast<const u8*>(bitmap.pixels)[3]);
    }

    auto& device = Renderer::GetDevice();
    RHI::TextureDesc texDesc{};
    texDesc.width = width;
    texDesc.height = height;
    texDesc.format = RHI::TextureFormat::RGBA8_UNORM;
    texDesc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
    RHI::TextureHandle texture = device.CreateTexture(texDesc);
    device.GetCurrentCommandBuffer().UpdateTexture(
        texture, std::span<const u8>(reinterpret_cast<const u8*>(bitmap.pixels), (size_t)width * height * N));
    return texture;
}

Font2D::Font2D(const std::string& ttfPath, u32 genSizePx, f32 pixelRange)
    : m_Path(ttfPath), m_GenSizePx(genSizePx), m_PixelRange(pixelRange), m_Impl(std::make_unique<Impl>())
{
    msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
    if (!ft)
    {
        UHE_CORE_ERROR("Failed to initialize FreeType for font: {}", m_Path);
        return;
    }

    msdfgen::FontHandle* font = msdfgen::loadFont(ft, m_Path.c_str());
    if (!font)
    {
        UHE_CORE_ERROR("Failed to load font: {}", m_Path);
        msdfgen::deinitializeFreetype(ft);
        return;
    }

    struct CharsetRange
    {
        u32 Begin, End;
    };

    static const CharsetRange charsetRanges[] = {{0x0020, 0x00FF}};

    msdf_atlas::Charset charset;
    for (CharsetRange range : charsetRanges)
    {
        for (u32 c = range.Begin; c <= range.End; c++)
            charset.add(c);
    }

    f64 fontScale = 1.0;
    i32 glyphsLoaded = m_Impl->FontGeometry.loadCharset(font, fontScale, charset);
    UHE_CORE_INFO("Loaded {} glyphs from font (out of {})", glyphsLoaded, charset.size());
    if (glyphsLoaded <= 0)
    {
        UHE_CORE_ERROR("No glyphs loaded from font: {}", m_Path);
        msdfgen::destroyFont(font);
        msdfgen::deinitializeFreetype(ft);
        return;
    }

    f64 emSize = static_cast<f64>(m_GenSizePx);

    msdf_atlas::TightAtlasPacker atlasPacker;
    atlasPacker.setPixelRange(m_PixelRange);
    atlasPacker.setMiterLimit(1.0);
    atlasPacker.setScale(emSize);
    i32 remaining = atlasPacker.pack(m_Impl->Glyphs.data(), static_cast<i32>(m_Impl->Glyphs.size()));
    if (remaining != 0)
    {
        UHE_CORE_ERROR("Atlas packing failed for font {}: {} glyphs did not fit", m_Path, remaining);
        msdfgen::destroyFont(font);
        msdfgen::deinitializeFreetype(ft);
        return;
    }

    i32 width, height;
    atlasPacker.getDimensions(width, height);
    m_AtlasWidth = static_cast<u32>(width);
    m_AtlasHeight = static_cast<u32>(height);

    constexpr f64 DEFAULT_ANGLE_THRESHOLD = 3.0;
    constexpr u64 LCG_MULTIPLIER = 6364136223846793005ull;
    constexpr u64 LCG_INCREMENT = 1442695040888963407ull;
    u64 glyphSeed = 0;
    for (msdf_atlas::GlyphGeometry& glyph : m_Impl->Glyphs)
    {
        glyphSeed = glyphSeed * LCG_MULTIPLIER + LCG_INCREMENT;
        glyph.edgeColoring(msdfgen::edgeColoringInkTrap, DEFAULT_ANGLE_THRESHOLD, glyphSeed);
    }

    m_Atlas = CreateAtlasTexture<u8, f32, 4, msdf_atlas::mtsdfGenerator>(m_Impl->Glyphs, width, height);
    if (!m_Atlas)
    {
        UHE_CORE_ERROR("Failed to create MSDF atlas for font: {}", m_Path);
        msdfgen::destroyFont(font);
        msdfgen::deinitializeFreetype(ft);
        return;
    }

    const msdfgen::FontMetrics& metrics = m_Impl->FontGeometry.getMetrics();
    m_Ascent = static_cast<f32>(metrics.ascenderY);
    m_LineHeight = static_cast<f32>(metrics.lineHeight);

    for (const msdf_atlas::GlyphGeometry& glyph : m_Impl->Glyphs)
    {
        i32 codepoint = glyph.getIdentifier(msdf_atlas::GlyphIdentifierType::UNICODE_CODEPOINT);
        if (codepoint < 0)
            continue;

        double planeL, planeB, planeR, planeT;
        glyph.getQuadPlaneBounds(planeL, planeB, planeR, planeT);
        double atlasL, atlasB, atlasR, atlasT;
        glyph.getQuadAtlasBounds(atlasL, atlasB, atlasR, atlasT);

        FontGlyph fontGlyph;
        fontGlyph.PlaneBoundsMin = {static_cast<f32>(planeL), static_cast<f32>(planeB)};
        fontGlyph.PlaneBoundsMax = {static_cast<f32>(planeR), static_cast<f32>(planeT)};
        fontGlyph.UVMin = {static_cast<f32>(atlasL) / static_cast<f32>(m_AtlasWidth),
                           static_cast<f32>(atlasT) / static_cast<f32>(m_AtlasHeight)};
        fontGlyph.UVMax = {static_cast<f32>(atlasR) / static_cast<f32>(m_AtlasWidth),
                           static_cast<f32>(atlasB) / static_cast<f32>(m_AtlasHeight)};
        fontGlyph.Advance = static_cast<f32>(glyph.getAdvance());

        m_Glyphs.emplace(static_cast<u32>(codepoint), fontGlyph);
    }

    m_Valid = true;

    UHE_CORE_INFO("Font loaded: {}, {} glyphs, atlas {}x{}", m_Path, static_cast<u32>(m_Glyphs.size()), m_AtlasWidth,
                  m_AtlasHeight);

    msdfgen::destroyFont(font);
    msdfgen::deinitializeFreetype(ft);
}

Font2D::~Font2D()
{
    if (m_Atlas)
        Renderer::GetDevice().DestroyTexture(m_Atlas);
}

Ref<Font2D> Font2D::Get(const std::string& ttfPath, u32 genSizePx)
{
    std::string key = ttfPath + "#" + std::to_string(genSizePx);
    auto it = s_Cache.find(key);
    if (it != s_Cache.end())
        return it->second;

    Ref<Font2D> font = CreateRef<Font2D>(ttfPath, genSizePx);
    if (!font->IsValid())
    {
        UHE_CORE_ERROR("Failed to create font: {} (not cached)", ttfPath);
        return nullptr;
    }

    s_Cache[key] = font;
    return font;
}

Ref<Font2D> Font2D::GetDefault()
{
    if (!s_DefaultFont)
    {
        s_DefaultFont = Get((FileSystem::Get().GetRootPath() / "assets/Inter/static/Inter_18pt-Bold.ttf").string());
    }
    return s_DefaultFont;
}

void Font2D::Shutdown()
{
    s_DefaultFont.reset();
    s_Cache.clear();
}

const FontGlyph* Font2D::GetGlyph(u32 codepoint) const
{
    auto it = m_Glyphs.find(codepoint);
    if (it != m_Glyphs.end())
        return &it->second;
    return nullptr;
}

std::unordered_map<std::string, Ref<Font2D>> Font2D::s_Cache;
Ref<Font2D> Font2D::s_DefaultFont;

} // namespace UHE