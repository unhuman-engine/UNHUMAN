#include "uhepch.h"
#include "UHE/Renderer/PostProcess.h"
#include <filesystem>
#include <map>
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
};

struct PostProcessData
{
    bool Loaded = false;
    bool ListenerRegistered = false;
    ShaderCache::Handle TonemapHandle = 0;
    RHI::ShaderHandle TonemapVertex = nullptr;
    RHI::ShaderHandle TonemapFragment = nullptr;
    u32 PushConstantSize = 0;
    std::map<RHI::TextureFormat, RHI::PipelineHandle> TonemapPipelines; ///< per output format
};

PostProcessData s_Post;

void DestroyResources()
{
    auto& device = Renderer::GetDevice();
    for (auto& [format, pipeline] : s_Post.TonemapPipelines)
        device.DestroyGraphicsPipeline(pipeline);
    s_Post.TonemapPipelines.clear();
    if (s_Post.TonemapVertex != nullptr)
        device.DestroyShader(s_Post.TonemapVertex);
    if (s_Post.TonemapFragment != nullptr)
        device.DestroyShader(s_Post.TonemapFragment);
    s_Post.TonemapVertex = nullptr;
    s_Post.TonemapFragment = nullptr;
    s_Post.Loaded = false;
}

/// @brief Loads the tonemap shader on first use. @return false when it is unavailable.
bool EnsureLoaded()
{
    if (s_Post.Loaded)
        return true;
    ShaderCache& library = ShaderCache::Get();
    const std::filesystem::path path = FileSystem::Get().GetRootPath() / "assets/shaders/Tonemap.slang";
    s_Post.TonemapHandle = library.Load(path.string());
    const ShaderCache::Shader* shader = library.Get(s_Post.TonemapHandle);
    if (shader == nullptr)
    {
        UHE_CORE_ERROR("PostProcess: Tonemap.slang failed to load; the tonemap pass will not draw");
        return false;
    }

    auto& device = Renderer::GetDevice();
    s_Post.TonemapVertex = CreateStageShader(device, *shader, RHI::ShaderStage::Vertex, "Tonemap.vertex");
    s_Post.TonemapFragment = CreateStageShader(device, *shader, RHI::ShaderStage::Fragment, "Tonemap.fragment");
    s_Post.PushConstantSize = ReflectedPushConstantSize(*shader);
    if (s_Post.PushConstantSize != sizeof(TonemapPushConstants))
        UHE_CORE_ERROR("PostProcess: Tonemap.slang push constants are {0} bytes, the host struct is {1}",
                       s_Post.PushConstantSize, sizeof(TonemapPushConstants));

    if (!s_Post.ListenerRegistered)
    {
        s_Post.ListenerRegistered = true;
        // Rebuilt lazily on the next use; queued frames still use the old objects.
        ShaderCache::Get().AddReloadListener(
            [](ShaderCache::Handle handle)
            {
                if (handle != s_Post.TonemapHandle || !s_Post.Loaded)
                    return;
                Renderer::GetDevice().WaitIdle();
                DestroyResources();
            });
    }
    s_Post.Loaded = s_Post.TonemapVertex != nullptr && s_Post.TonemapFragment != nullptr;
    return s_Post.Loaded;
}

RHI::PipelineHandle TonemapPipeline(RHI::TextureFormat format)
{
    const auto it = s_Post.TonemapPipelines.find(format);
    if (it != s_Post.TonemapPipelines.end())
        return it->second;

    RHI::GraphicsPipelineDesc desc{};
    desc.vertexShader = s_Post.TonemapVertex;
    desc.fragmentShader = s_Post.TonemapFragment;
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0] = format;
    desc.depthTest = false; // no depth attachment: the pipeline's depth format stays undefined
    desc.depthWrite = false;
    desc.blendMode = RHI::BlendMode::None;
    desc.cullMode = RHI::CullMode::None;
    desc.pushConstantSize = s_Post.PushConstantSize;
    desc.debugName = "PostProcess.Tonemap";
    const RHI::PipelineHandle pipeline = Renderer::GetDevice().CreateGraphicsPipeline(desc);
    s_Post.TonemapPipelines.emplace(format, pipeline);
    return pipeline;
}
} // namespace

RHI::FGHandle<RHI::FGTexture> PostProcess::AddTonemapPass(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> hdr,
                                                          RHI::FGHandle<RHI::FGTexture> target,
                                                          RHI::TextureFormat targetFormat,
                                                          const TonemapSettings& settings)
{
    using namespace RHI;
    struct TonemapData
    {
        FGHandle<FGTexture> Hdr;
        FGHandle<FGTexture> Output;
    };
    const TonemapData& data = graph.AddPass<TonemapData>(
        "PostProcess.Tonemap",
        [&](FGBuilder& builder, TonemapData& d)
        {
            d.Hdr = builder.Read(hdr);
            // Every pixel is written, so the target's old contents are not loaded.
            FGAttachment overwrite;
            overwrite.Load = LoadOp::Clear;
            d.Output = builder.Write(target, FGWrite::RenderTarget, overwrite);
        },
        [targetFormat, settings](const TonemapData& d, FGContext& context)
        {
            if (!EnsureLoaded())
                return;
            const TextureHandle hdrTexture = context.Get(d.Hdr);
            if (hdrTexture == nullptr)
                return;

            RHICommandBuffer& cmd = context.Cmd();
            cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
            cmd.SetScissor(0, 0, context.Width(), context.Height());
            cmd.BindPipeline(TonemapPipeline(targetFormat));

            TonemapPushConstants pc;
            pc.HdrTexture = reinterpret_cast<RHITexture*>(hdrTexture)->GetTextureIndex();
            pc.Exposure = settings.Exposure;
            cmd.PushConstants(ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
            cmd.Draw(3, 0);
        });
    return data.Output;
}

void PostProcess::Shutdown()
{
    DestroyResources();
}

} // namespace UHE
