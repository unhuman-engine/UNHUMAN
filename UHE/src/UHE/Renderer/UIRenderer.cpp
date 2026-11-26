#include "uhepch.h"
#include "UHE/Renderer/UIRenderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/RHI/RHITexture.h"
#include "UHE/Renderer/Font.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer/ShaderResources.h"
#include "UHE/Renderer/Texture.h"

namespace UHE
{

namespace
{
/// Must match `VSInput` in UI.slang (its layout is reflected from the module).
struct UIVertex
{
    glm::vec2 Position;
    glm::vec2 UV;
    glm::vec4 Color;
    glm::vec4 Rect;
    glm::vec4 Params;
    glm::vec4 BorderColor;
};

/// Must match `PushConstants` in UI.slang; checked against reflection.
struct UIPushConstants
{
    glm::vec2 ScreenSize{1.0f};
    f32 FlipY = 0.0f;
    f32 Padding = 0.0f;
};
static_assert(sizeof(UIPushConstants) == 16, "UIPushConstants must match UI.slang");

constexpr u32 kFramesInFlight = 3;

struct UIData
{
    bool Loaded = false;
    bool Failed = false;
    bool ListenerRegistered = false;
    ShaderCache::Handle Handle = 0;
    RHI::ShaderHandle Vertex = nullptr;
    RHI::ShaderHandle Fragment = nullptr;
    RHI::GraphicsPipelineDesc Desc{};
    std::unordered_map<RHI::TextureFormat, RHI::PipelineHandle> Pipelines;

    /// One vertex buffer per frame in flight: a frame still on the GPU keeps reading its own.
    std::array<RHI::BufferHandle, kFramesInFlight> Buffers{};
    std::array<u64, kFramesInFlight> Capacity{};

    std::unordered_map<std::string, Ref<Texture2D>> Textures; ///< by asset path; a failed load is kept as null
};

UIData s_UI;

void DestroyPipelines()
{
    auto& device = Renderer::GetDevice();
    for (auto& [format, pipeline] : s_UI.Pipelines)
        device.DestroyGraphicsPipeline(pipeline);
    s_UI.Pipelines.clear();
    if (s_UI.Vertex != nullptr)
        device.DestroyShader(s_UI.Vertex);
    if (s_UI.Fragment != nullptr)
        device.DestroyShader(s_UI.Fragment);
    s_UI.Vertex = nullptr;
    s_UI.Fragment = nullptr;
    s_UI.Loaded = false;
}

bool EnsureLoaded()
{
    if (s_UI.Loaded)
        return true;
    if (s_UI.Failed)
        return false;
    ShaderCache& library = ShaderCache::Get();
    const std::filesystem::path path = FileSystem::Get().GetRootPath() / "assets/shaders/UI.slang";
    s_UI.Handle = library.Load(path.string());
    const ShaderCache::Shader* shader = library.Get(s_UI.Handle);
    if (shader == nullptr)
    {
        UHE_CORE_ERROR("UI: UI.slang failed to load; the game UI is not drawn");
        s_UI.Failed = true;
        return false;
    }
    auto& device = Renderer::GetDevice();
    s_UI.Vertex = CreateStageShader(device, *shader, RHI::ShaderStage::Vertex, "UI.vertex");
    s_UI.Fragment = CreateStageShader(device, *shader, RHI::ShaderStage::Fragment, "UI.fragment");
    RHI::GraphicsPipelineDesc desc{};
    desc.vertexShader = s_UI.Vertex;
    desc.fragmentShader = s_UI.Fragment;
    desc.vertexLayout = ReflectedVertexLayout(*shader);
    desc.pushConstantSize = ReflectedPushConstantSize(*shader);
    if (desc.pushConstantSize != sizeof(UIPushConstants))
        UHE_CORE_ERROR("UI: UI.slang push constants are {0} bytes, the host struct is {1}", desc.pushConstantSize,
                       sizeof(UIPushConstants));
    desc.colorAttachmentCount = 1;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.blendMode = RHI::BlendMode::Alpha;
    desc.cullMode = RHI::CullMode::None;
    desc.debugName = "UI";
    s_UI.Desc = desc;

    if (!s_UI.ListenerRegistered)
    {
        s_UI.ListenerRegistered = true;
        ShaderCache::Get().AddReloadListener(
            [](ShaderCache::Handle handle)
            {
                if (handle != s_UI.Handle || !s_UI.Loaded)
                    return;
                Renderer::GetDevice().WaitIdle();
                DestroyPipelines();
            });
    }
    s_UI.Loaded = s_UI.Vertex != nullptr && s_UI.Fragment != nullptr;
    return s_UI.Loaded;
}

RHI::PipelineHandle PipelineFor(RHI::TextureFormat format)
{
    if (const auto it = s_UI.Pipelines.find(format); it != s_UI.Pipelines.end())
        return it->second;
    RHI::GraphicsPipelineDesc desc = s_UI.Desc;
    desc.colorFormats[0] = format;
    return s_UI.Pipelines.emplace(format, Renderer::GetDevice().CreateGraphicsPipeline(desc)).first->second;
}

float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// UI colours are authored in sRGB; the target encodes on write, so the shader works in linear.
glm::vec4 Linear(const glm::vec4& c)
{
    return {SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b), c.a};
}

i32 TextureIndexFor(const std::string& assetPath)
{
    if (assetPath.empty())
        return -1;
    auto it = s_UI.Textures.find(assetPath);
    if (it == s_UI.Textures.end())
    {
        Ref<Texture2D> texture;
        const std::filesystem::path resolved = ResolveAssetPath(assetPath);
        std::error_code ec;
        if (std::filesystem::is_regular_file(resolved, ec))
            texture = Texture2D::Create(resolved.string());
        else
            UHE_CORE_ERROR("UI: the image '{0}' was not found", assetPath);
        it = s_UI.Textures.emplace(assetPath, texture).first;
    }
    return it->second ? static_cast<i32>(it->second->GetTextureIndex()) : -1;
}

Ref<Font2D> FontFor(const std::string& assetPath)
{
    if (assetPath.empty())
        return Font2D::GetDefault();
    const std::filesystem::path resolved = ResolveAssetPath(assetPath);
    Ref<Font2D> font = Font2D::Get(resolved.string());
    return font && font->IsValid() ? font : Font2D::GetDefault();
}

void PushQuad(std::vector<UIVertex>& out, glm::vec2 min, glm::vec2 max, glm::vec2 uvMin, glm::vec2 uvMax,
              const UIVertex& shared)
{
    const glm::vec2 centre = (min + max) * 0.5f;
    const std::array<glm::vec2, 4> corners = {min, {max.x, min.y}, max, {min.x, max.y}};
    const std::array<glm::vec2, 4> uvs = {uvMin, {uvMax.x, uvMin.y}, uvMax, {uvMin.x, uvMax.y}};
    for (const int i : {0, 1, 2, 0, 2, 3})
    {
        UIVertex v = shared;
        v.Position = corners[static_cast<size_t>(i)];
        v.UV = uvs[static_cast<size_t>(i)];
        v.Rect.x = corners[static_cast<size_t>(i)].x - centre.x;
        v.Rect.y = corners[static_cast<size_t>(i)].y - centre.y;
        out.push_back(v);
    }
}

/// The next UTF-8 code point at \p i (advanced past it); U+FFFD for a broken sequence.
char32_t NextCodePoint(const std::string& text, size_t& i)
{
    const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char c0 = byte(i);
    int length = 1;
    char32_t code = c0;
    if ((c0 & 0xE0) == 0xC0)
        length = 2, code = c0 & 0x1F;
    else if ((c0 & 0xF0) == 0xE0)
        length = 3, code = c0 & 0x0F;
    else if ((c0 & 0xF8) == 0xF0)
        length = 4, code = c0 & 0x07;
    else if (c0 & 0x80)
    {
        ++i;
        return 0xFFFD;
    }
    if (i + static_cast<size_t>(length) > text.size())
    {
        i = text.size();
        return 0xFFFD;
    }
    for (int k = 1; k < length; ++k)
        code = (code << 6) | (byte(i + static_cast<size_t>(k)) & 0x3F);
    i += static_cast<size_t>(length);
    return code;
}

void AddText(std::vector<UIVertex>& out, const UI::DrawCommand& command)
{
    const Ref<Font2D> font = FontFor(command.FontPath);
    if (!font || !font->IsValid() || font->GetAtlas() == nullptr || font->GetLineHeight() <= 0.0f)
        return;
    const float size = command.FontSize;
    const float scale = size / font->GetLineHeight(); // font units to pixels

    // Split into lines and measure each, for the alignment.
    std::vector<std::string> lines(1);
    for (const char c : command.Text)
    {
        if (c == '\n')
            lines.emplace_back();
        else
            lines.back() += c;
    }
    const auto measure = [&](const std::string& line)
    {
        float width = 0.0f;
        for (size_t i = 0; i < line.size();)
            if (const FontGlyph* glyph = font->GetGlyph(static_cast<u32>(NextCodePoint(line, i))))
                width += glyph->Advance * scale;
        return width;
    };

    UIVertex shared{};
    shared.Color = Linear(command.Color);
    shared.Params =
        glm::vec4(font->GetPixelRange() / static_cast<float>(std::max(font->GetAtlasWidth(), 1u)), 0.0f, 1.0f,
                  static_cast<float>(reinterpret_cast<RHI::RHITexture*>(font->GetAtlas())->GetTextureIndex()));

    // The block is centred vertically in the rect; each line's baseline sits an ascent below its top.
    const float blockHeight = size * static_cast<float>(lines.size());
    float top = (command.Min.y + command.Max.y) * 0.5f - blockHeight * 0.5f;
    const float ascent = font->GetAscent() > 0.0f ? font->GetAscent() * scale : size * 0.8f;
    for (const std::string& line : lines)
    {
        const float width = measure(line);
        float pen = command.Min.x;
        if (command.Align == 1)
            pen = (command.Min.x + command.Max.x - width) * 0.5f;
        else if (command.Align == 2)
            pen = command.Max.x - width;
        const float baseline = top + ascent;
        for (size_t i = 0; i < line.size();)
        {
            const FontGlyph* glyph = font->GetGlyph(static_cast<u32>(NextCodePoint(line, i)));
            if (glyph == nullptr)
                continue;
            const glm::vec2 min(pen + glyph->PlaneBoundsMin.x * scale, baseline - glyph->PlaneBoundsMax.y * scale);
            const glm::vec2 max(pen + glyph->PlaneBoundsMax.x * scale, baseline - glyph->PlaneBoundsMin.y * scale);
            if (max.x > min.x && max.y > min.y)
                PushQuad(out, min, max, glyph->UVMin, glyph->UVMax, shared);
            pen += glyph->Advance * scale;
        }
        top += size;
    }
}

void AddBox(std::vector<UIVertex>& out, const UI::DrawCommand& command)
{
    const glm::vec2 half = (command.Max - command.Min) * 0.5f;
    if (half.x <= 0.0f || half.y <= 0.0f)
        return;
    UIVertex shared{};
    shared.Color = Linear(command.Color);
    shared.BorderColor = Linear(command.BorderColor);
    shared.Rect = glm::vec4(0.0f, 0.0f, half);
    shared.Params = glm::vec4(std::max(command.CornerRadius, 0.0f), std::max(command.BorderWidth, 0.0f), 0.0f,
                              static_cast<float>(TextureIndexFor(command.TexturePath)));
    // Image rows were flipped on load: v = 1 is the image's top.
    PushQuad(out, command.Min, command.Max, {0.0f, 1.0f}, {1.0f, 0.0f}, shared);
}
} // namespace

RHI::FGHandle<RHI::FGTexture> UIRenderer::AddPass(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> target,
                                                  RHI::TextureFormat format, u32 width, u32 height, bool flipY,
                                                  const std::vector<UI::DrawCommand>& commands)
{
    using namespace RHI;
    if (commands.empty() || !target.IsValid() || width == 0 || height == 0 || !EnsureLoaded())
        return target;

    // Built now, while uploads are allowed: textures and font atlases load here.
    auto vertices = std::make_shared<std::vector<UIVertex>>();
    vertices->reserve(commands.size() * 6);
    for (const UI::DrawCommand& command : commands)
    {
        if (command.Type == UI::DrawCommand::Kind::Text)
            AddText(*vertices, command);
        else
            AddBox(*vertices, command);
    }
    if (vertices->empty())
        return target;

    struct UIPass
    {
        FGHandle<FGTexture> Color;
    };
    const RHI::PipelineHandle pipeline = PipelineFor(format);
    const UIPass& pass = graph.AddPass<UIPass>(
        "UI",
        [&](FGBuilder& builder, UIPass& d)
        {
            FGAttachment keep;
            keep.Load = LoadOp::Load;
            d.Color = builder.Write(target, FGWrite::RenderTarget, keep);
        },
        [vertices, pipeline, width, height, flipY](const UIPass&, FGContext& context)
        {
            auto& device = Renderer::GetDevice();
            const u32 frame = device.GetCurrentFrameIndex() % kFramesInFlight;
            const u64 bytes = vertices->size() * sizeof(UIVertex);
            if (s_UI.Capacity[frame] < bytes)
            {
                if (s_UI.Buffers[frame] != nullptr)
                    device.DestroyBuffer(s_UI.Buffers[frame]); // deferred until the frame using it is done
                RHI::BufferDesc desc{};
                desc.size = std::max<u64>(bytes * 2, u64{64} * 1024);
                desc.usage = RHI::BufferUsage::Vertex;
                desc.hostVisible = true;
                desc.debugName = "UI.Vertices";
                s_UI.Buffers[frame] = device.CreateBuffer(desc);
                s_UI.Capacity[frame] = desc.size;
            }
            RHICommandBuffer& cmd = context.Cmd();
            cmd.UpdateBuffer(s_UI.Buffers[frame], vertices->data(), bytes, 0);
            cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
            cmd.SetScissor(0, 0, context.Width(), context.Height());
            cmd.BindPipeline(pipeline);
            cmd.BindVertexBuffer(s_UI.Buffers[frame], 0);
            UIPushConstants pc;
            pc.ScreenSize = glm::vec2(static_cast<f32>(width), static_cast<f32>(height));
            pc.FlipY = flipY ? 1.0f : 0.0f;
            cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
            cmd.Draw(static_cast<u32>(vertices->size()), 0);
        });
    return pass.Color;
}

void UIRenderer::Shutdown()
{
    auto& device = Renderer::GetDevice();
    DestroyPipelines();
    for (u32 i = 0; i < kFramesInFlight; ++i)
    {
        if (s_UI.Buffers[i] != nullptr)
            device.DestroyBuffer(s_UI.Buffers[i]);
        s_UI.Buffers[i] = nullptr;
        s_UI.Capacity[i] = 0;
    }
    s_UI.Textures.clear();
    s_UI.Failed = false;
}

} // namespace UHE
