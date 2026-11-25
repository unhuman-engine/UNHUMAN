#include "uhepch.h"
#include "UHE/Renderer/PostProcess.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/RHI/RHITexture.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer/ShaderResources.h"

namespace UHE
{

namespace
{
/// Must match `PushConstants` in Tonemap.slang; checked against reflection.
struct TonemapPushConstants
{
    u32 HdrTexture = 0;
    f32 Exposure = 1.0f;
    i32 BloomTexture = -1; ///< bindless slot, or -1 for no bloom
    f32 BloomIntensity = 0.0f;
};

/// Must match `PushConstants` in Bloom.slang; checked against reflection.
struct BloomPushConstants
{
    glm::vec2 SrcTexelSize{0.0f}; ///< 1 / size of the source level, in uv
    u32 SrcTexture = 0;
    i32 BaseTexture = -1; ///< upsample: this level's downsample, added to the filtered image
    u32 Mode = 0;         ///< a BloomMode
    f32 FilterRadius = 1.0f;
};

/// The filter Bloom.slang runs; its entry points are fixed, so one pipeline serves every pass.
enum class BloomMode : u8
{
    Downsample = 0,
    DownsampleKaris = 1, ///< first level only: kills fireflies before they spread down the chain
    Upsample = 2,
};

/// Levels the chain stops at; past this the blur is wide enough that more levels add only cost.
constexpr u32 kMaxBloomLevels = 6;
/// A level smaller than this on either side is not made: the 13-tap filter would mostly read clamped edges.
constexpr u32 kMinBloomLevelSize = 8;

/// One shader's two stages and the push constant size its module declares.
struct Program
{
    ShaderCache::Handle Handle = 0;
    RHI::ShaderHandle Vertex = nullptr;
    RHI::ShaderHandle Fragment = nullptr;
    u32 PushConstantSize = 0;
    bool Loaded = false;
};

struct PostProcessData
{
    bool ListenerRegistered = false;
    Program Tonemap;
    Program Bloom;
    std::map<RHI::TextureFormat, RHI::PipelineHandle> TonemapPipelines; ///< per output format
    RHI::PipelineHandle BloomPipeline = nullptr;                        ///< every bloom level is RGBA16F
};

PostProcessData s_Post;

void DestroyProgram(Program& program)
{
    auto& device = Renderer::GetDevice();
    if (program.Vertex != nullptr)
        device.DestroyShader(program.Vertex);
    if (program.Fragment != nullptr)
        device.DestroyShader(program.Fragment);
    program.Vertex = nullptr;
    program.Fragment = nullptr;
    program.Loaded = false;
}

void DestroyTonemap()
{
    auto& device = Renderer::GetDevice();
    for (auto& [format, pipeline] : s_Post.TonemapPipelines)
        device.DestroyGraphicsPipeline(pipeline);
    s_Post.TonemapPipelines.clear();
    DestroyProgram(s_Post.Tonemap);
}

void DestroyBloom()
{
    if (s_Post.BloomPipeline != nullptr)
        Renderer::GetDevice().DestroyGraphicsPipeline(s_Post.BloomPipeline);
    s_Post.BloomPipeline = nullptr;
    DestroyProgram(s_Post.Bloom);
}

void DestroyResources()
{
    DestroyTonemap();
    DestroyBloom();
}

void RegisterReloadListener()
{
    if (s_Post.ListenerRegistered)
        return;
    s_Post.ListenerRegistered = true;
    // Rebuilt lazily on the next use; queued frames still use the old objects.
    ShaderCache::Get().AddReloadListener(
        [](ShaderCache::Handle handle)
        {
            if (handle == s_Post.Tonemap.Handle && s_Post.Tonemap.Loaded)
            {
                Renderer::GetDevice().WaitIdle();
                DestroyTonemap();
            }
            else if (handle == s_Post.Bloom.Handle && s_Post.Bloom.Loaded)
            {
                Renderer::GetDevice().WaitIdle();
                DestroyBloom();
            }
        });
}

/**
 * @brief Loads \p name from assets/shaders into \p program on first use.
 * @param hostPushConstantSize sizeof the host struct the passes push; a mismatch with the module is logged
 * @return false when the shader is unavailable.
 */
bool EnsureLoaded(Program& program, const char* name, u32 hostPushConstantSize)
{
    if (program.Loaded)
        return true;
    ShaderCache& library = ShaderCache::Get();
    const std::filesystem::path path = FileSystem::Get().GetRootPath() / "assets/shaders" / name;
    program.Handle = library.Load(path.string());
    const ShaderCache::Shader* shader = library.Get(program.Handle);
    if (shader == nullptr)
    {
        UHE_CORE_ERROR("PostProcess: {0} failed to load; its passes will not draw", name);
        return false;
    }

    auto& device = Renderer::GetDevice();
    const std::string stem = std::filesystem::path(name).stem().string();
    program.Vertex = CreateStageShader(device, *shader, RHI::ShaderStage::Vertex, (stem + ".vertex").c_str());
    program.Fragment = CreateStageShader(device, *shader, RHI::ShaderStage::Fragment, (stem + ".fragment").c_str());
    program.PushConstantSize = ReflectedPushConstantSize(*shader);
    if (program.PushConstantSize != hostPushConstantSize)
        UHE_CORE_ERROR("PostProcess: {0} push constants are {1} bytes, the host struct is {2}", name,
                       program.PushConstantSize, hostPushConstantSize);

    RegisterReloadListener();
    program.Loaded = program.Vertex != nullptr && program.Fragment != nullptr;
    // A half-built program is retried on the next use: release the stage that did build so it does not leak.
    if (!program.Loaded)
        DestroyProgram(program);
    return program.Loaded;
}

/// @brief A fullscreen-triangle pipeline: one colour attachment, no depth, no blending.
RHI::PipelineHandle CreateFullscreenPipeline(const Program& program, RHI::TextureFormat format, const char* debugName)
{
    RHI::GraphicsPipelineDesc desc{};
    desc.vertexShader = program.Vertex;
    desc.fragmentShader = program.Fragment;
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0] = format;
    desc.depthTest = false; // no depth attachment: the pipeline's depth format stays undefined
    desc.depthWrite = false;
    desc.blendMode = RHI::BlendMode::None;
    desc.cullMode = RHI::CullMode::None;
    desc.pushConstantSize = program.PushConstantSize;
    desc.debugName = debugName;
    return Renderer::GetDevice().CreateGraphicsPipeline(desc);
}

RHI::PipelineHandle TonemapPipeline(RHI::TextureFormat format)
{
    const auto it = s_Post.TonemapPipelines.find(format);
    if (it != s_Post.TonemapPipelines.end())
        return it->second;
    const RHI::PipelineHandle pipeline = CreateFullscreenPipeline(s_Post.Tonemap, format, "PostProcess.Tonemap");
    s_Post.TonemapPipelines.emplace(format, pipeline);
    return pipeline;
}

RHI::PipelineHandle BloomPipeline()
{
    if (s_Post.BloomPipeline == nullptr)
        s_Post.BloomPipeline = CreateFullscreenPipeline(s_Post.Bloom, RHI::TextureFormat::RGBA16F, "PostProcess.Bloom");
    return s_Post.BloomPipeline;
}

/// @brief What one bloom pass filters, fixed at declaration time.
struct BloomPassParams
{
    BloomMode Mode = BloomMode::Downsample;
    u32 SourceWidth = 0; ///< size of the level being filtered, for its texel size
    u32 SourceHeight = 0;
    f32 FilterRadius = 1.0f;
};

/**
 * @brief Declares one bloom pass: filters \p source (plus \p base when upsampling) into a new texture \p outputName.
 *
 * Every pass writes a texture of its own and only samples the others, so the
 * graph sees a plain chain of reads and writes and never a feedback loop.
 */
RHI::FGHandle<RHI::FGTexture> AddBloomPass(RHI::FrameGraph& graph, const std::string& passName,
                                           const std::string& outputName, u32 width, u32 height,
                                           RHI::FGHandle<RHI::FGTexture> source, RHI::FGHandle<RHI::FGTexture> base,
                                           const BloomPassParams& params)
{
    using namespace RHI;
    struct BloomData
    {
        FGHandle<FGTexture> Source;
        FGHandle<FGTexture> Base;
        FGHandle<FGTexture> Output;
    };
    const BloomData& data = graph.AddPass<BloomData>(
        passName,
        [&](FGBuilder& builder, BloomData& d)
        {
            d.Source = builder.Read(source);
            if (base.IsValid())
                d.Base = builder.Read(base);
            // Every pixel is written, so nothing needs loading.
            FGAttachment overwrite;
            overwrite.Load = LoadOp::Clear;
            d.Output = builder.Write(
                builder.Create(outputName, {.Width = width, .Height = height, .Format = TextureFormat::RGBA16F}),
                FGWrite::RenderTarget, overwrite);
        },
        [params](const BloomData& d, FGContext& context)
        {
            if (!EnsureLoaded(s_Post.Bloom, "Bloom.slang", sizeof(BloomPushConstants)))
                return;
            const TextureHandle sourceTexture = context.Get(d.Source);
            if (sourceTexture == nullptr)
                return;
            const TextureHandle baseTexture = d.Base.IsValid() ? context.Get(d.Base) : nullptr;

            RHICommandBuffer& cmd = context.Cmd();
            cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
            cmd.SetScissor(0, 0, context.Width(), context.Height());
            cmd.BindPipeline(BloomPipeline());

            BloomPushConstants pc;
            pc.SrcTexelSize = {1.0f / static_cast<f32>(params.SourceWidth),
                               1.0f / static_cast<f32>(params.SourceHeight)};
            pc.SrcTexture = reinterpret_cast<RHITexture*>(sourceTexture)->GetTextureIndex();
            pc.BaseTexture = baseTexture != nullptr
                                 ? static_cast<i32>(reinterpret_cast<RHITexture*>(baseTexture)->GetTextureIndex())
                                 : -1;
            pc.Mode = static_cast<u32>(params.Mode);
            pc.FilterRadius = params.FilterRadius;
            cmd.PushConstants(ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
            cmd.Draw(3, 0);
        });
    return data.Output;
}
} // namespace

namespace
{
/// @brief The tonemap pass into \p target, or into a texture it creates as \p createName when \p target is invalid.
RHI::FGHandle<RHI::FGTexture> TonemapInto(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> hdr,
                                          RHI::FGHandle<RHI::FGTexture> target, const std::string& createName,
                                          const RHI::FGTexture::Desc& createDesc, RHI::TextureFormat targetFormat,
                                          const TonemapSettings& settings, RHI::FGHandle<RHI::FGTexture> bloom)
{
    using namespace RHI;
    struct TonemapData
    {
        FGHandle<FGTexture> Hdr;
        FGHandle<FGTexture> Bloom;
        FGHandle<FGTexture> Output;
    };
    const TonemapData& data = graph.AddPass<TonemapData>(
        "PostProcess.Tonemap",
        [&](FGBuilder& builder, TonemapData& d)
        {
            d.Hdr = builder.Read(hdr);
            if (bloom.IsValid())
                d.Bloom = builder.Read(bloom);
            // Every pixel is written, so the target's old contents are not loaded.
            FGAttachment overwrite;
            overwrite.Load = LoadOp::Clear;
            const FGHandle<FGTexture> output = target.IsValid() ? target : builder.Create(createName, createDesc);
            d.Output = builder.Write(output, FGWrite::RenderTarget, overwrite);
        },
        [targetFormat, settings](const TonemapData& d, FGContext& context)
        {
            if (!EnsureLoaded(s_Post.Tonemap, "Tonemap.slang", sizeof(TonemapPushConstants)))
                return;
            const TextureHandle hdrTexture = context.Get(d.Hdr);
            if (hdrTexture == nullptr)
                return;
            const TextureHandle bloomTexture = d.Bloom.IsValid() ? context.Get(d.Bloom) : nullptr;

            RHICommandBuffer& cmd = context.Cmd();
            cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
            cmd.SetScissor(0, 0, context.Width(), context.Height());
            cmd.BindPipeline(TonemapPipeline(targetFormat));

            TonemapPushConstants pc;
            pc.HdrTexture = reinterpret_cast<RHITexture*>(hdrTexture)->GetTextureIndex();
            pc.Exposure = settings.Exposure;
            pc.BloomTexture = bloomTexture != nullptr
                                  ? static_cast<i32>(reinterpret_cast<RHITexture*>(bloomTexture)->GetTextureIndex())
                                  : -1;
            pc.BloomIntensity = settings.BloomIntensity;
            cmd.PushConstants(ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
            cmd.Draw(3, 0);
        });
    return data.Output;
}
} // namespace

RHI::FGHandle<RHI::FGTexture> PostProcess::AddTonemapPass(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> hdr,
                                                          RHI::FGHandle<RHI::FGTexture> target,
                                                          RHI::TextureFormat targetFormat,
                                                          const TonemapSettings& settings,
                                                          RHI::FGHandle<RHI::FGTexture> bloom)
{
    return TonemapInto(graph, hdr, target, {}, {}, targetFormat, settings, bloom);
}

RHI::FGHandle<RHI::FGTexture> PostProcess::AddTonemapPass(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> hdr,
                                                          const std::string& targetName,
                                                          const RHI::FGTexture::Desc& targetDesc,
                                                          const TonemapSettings& settings,
                                                          RHI::FGHandle<RHI::FGTexture> bloom)
{
    return TonemapInto(graph, hdr, {}, targetName, targetDesc, targetDesc.Format, settings, bloom);
}

RHI::FGHandle<RHI::FGTexture> PostProcess::AddBloomPasses(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> hdr,
                                                          u32 width, u32 height, const BloomSettings& settings)
{
    using namespace RHI;
    if (!settings.Enabled || !hdr.IsValid())
        return {};

    // Level i is (width, height) >> (i + 1); the chain ends before a side drops below the minimum.
    struct Level
    {
        u32 Width = 0;
        u32 Height = 0;
        FGHandle<FGTexture> Down;
    };
    std::vector<Level> levels;
    for (u32 i = 0; i < kMaxBloomLevels; ++i)
    {
        const u32 levelWidth = width >> (i + 1);
        const u32 levelHeight = height >> (i + 1);
        if (levelWidth < kMinBloomLevelSize || levelHeight < kMinBloomLevelSize)
            break;
        levels.push_back({.Width = levelWidth, .Height = levelHeight, .Down = {}});
    }
    if (levels.empty())
        return {};

    // Down: each level filters the one above it; the first reads the full-resolution HDR image.
    FGHandle<FGTexture> source = hdr;
    u32 sourceWidth = width;
    u32 sourceHeight = height;
    for (u32 i = 0; i < static_cast<u32>(levels.size()); ++i)
    {
        BloomPassParams params;
        params.Mode = i == 0 ? BloomMode::DownsampleKaris : BloomMode::Downsample;
        params.SourceWidth = sourceWidth;
        params.SourceHeight = sourceHeight;
        const std::string index = std::to_string(i);
        levels[i].Down = AddBloomPass(graph, "PostProcess.Bloom.Downsample" + index, "Bloom.Down" + index,
                                      levels[i].Width, levels[i].Height, source, {}, params);
        source = levels[i].Down;
        sourceWidth = levels[i].Width;
        sourceHeight = levels[i].Height;
    }

    // Up: the smallest level is its own upsampled image; every larger one adds
    // the tent-filtered level below it to its own downsample.
    FGHandle<FGTexture> up = levels.back().Down;
    for (u32 i = static_cast<u32>(levels.size()) - 1; i-- > 0;)
    {
        BloomPassParams params;
        params.Mode = BloomMode::Upsample;
        params.SourceWidth = levels[i + 1].Width;
        params.SourceHeight = levels[i + 1].Height;
        params.FilterRadius = std::max(settings.FilterRadius, 0.0f);
        const std::string index = std::to_string(i);
        up = AddBloomPass(graph, "PostProcess.Bloom.Upsample" + index, "Bloom.Up" + index, levels[i].Width,
                          levels[i].Height, up, levels[i].Down, params);
    }
    return up;
}

void PostProcess::Shutdown()
{
    DestroyResources();
}

} // namespace UHE
