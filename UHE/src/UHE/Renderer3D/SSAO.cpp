#include "uhepch.h"
#include "UHE/Renderer3D/SSAO.h"
#include <filesystem>
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
/// Must match `PushConstants` in SSAO.slang; checked against reflection.
struct SSAOPushConstants
{
    glm::mat4 Projection{1.0f};
    glm::mat4 InverseProjection{1.0f};
    i32 DepthTexture = -1;
    i32 SourceTexture = -1;
    i32 Mode = 0; ///< 0: occlusion, 1: blur
    f32 Radius = 0.5f;
    f32 Bias = 0.025f;
    f32 Intensity = 1.0f;
    f32 Padding[2]{};
};
static_assert(sizeof(SSAOPushConstants) == 160, "SSAOPushConstants must match SSAO.slang");

struct SSAOData
{
    bool Loaded = false;
    bool Reported = false; ///< a failure was logged; again only after the next reload
    bool ListenerRegistered = false;
    ShaderCache::Handle Handle = 0;
    RHI::ShaderHandle Vertex = nullptr;
    RHI::ShaderHandle Fragment = nullptr;
    RHI::PipelineHandle Pipeline = nullptr;
};

SSAOData s_SSAO;

void DestroyResources()
{
    auto& device = Renderer::GetDevice();
    if (s_SSAO.Pipeline != nullptr)
        device.DestroyGraphicsPipeline(s_SSAO.Pipeline);
    if (s_SSAO.Vertex != nullptr)
        device.DestroyShader(s_SSAO.Vertex);
    if (s_SSAO.Fragment != nullptr)
        device.DestroyShader(s_SSAO.Fragment);
    s_SSAO.Pipeline = nullptr;
    s_SSAO.Vertex = nullptr;
    s_SSAO.Fragment = nullptr;
    s_SSAO.Loaded = false;
}

/// @brief Loads the shader and builds the pipeline on first use. @return false when unavailable.
bool EnsureLoaded()
{
    if (s_SSAO.Loaded)
        return true;
    ShaderCache& library = ShaderCache::Get();
    const std::filesystem::path path = FileSystem::Get().GetRootPath() / "assets/shaders/SSAO.slang";
    s_SSAO.Handle = library.Load(path.string());
    if (!s_SSAO.ListenerRegistered)
    {
        s_SSAO.ListenerRegistered = true;
        // Rebuilt lazily on the next use; queued frames still use the old objects.
        // 0 is ShaderCache::ReloadAll.
        ShaderCache::Get().AddReloadListener(
            [](ShaderCache::Handle handle)
            {
                if (handle != 0 && handle != s_SSAO.Handle)
                    return;
                s_SSAO.Reported = false;
                if (!s_SSAO.Loaded)
                    return;
                Renderer::GetDevice().WaitIdle();
                DestroyResources();
            });
    }
    const ShaderCache::Shader* shader = library.Get(s_SSAO.Handle);
    if (shader == nullptr)
    {
        if (!s_SSAO.Reported)
            UHE_CORE_ERROR("SSAO: SSAO.slang failed to load; the scene draws without occlusion");
        s_SSAO.Reported = true;
        return false;
    }
    // Draw pushes the whole host struct; a smaller declared range would be overrun.
    const u32 pushConstantSize = ReflectedPushConstantSize(*shader);
    if (pushConstantSize != sizeof(SSAOPushConstants))
    {
        if (!s_SSAO.Reported)
            UHE_CORE_ERROR("SSAO: SSAO.slang push constants are {0} bytes, the host struct is {1}; the scene draws "
                           "without occlusion",
                           pushConstantSize, sizeof(SSAOPushConstants));
        s_SSAO.Reported = true;
        return false;
    }

    auto& device = Renderer::GetDevice();
    s_SSAO.Vertex = CreateStageShader(device, *shader, RHI::ShaderStage::Vertex, "SSAO.vertex");
    s_SSAO.Fragment = CreateStageShader(device, *shader, RHI::ShaderStage::Fragment, "SSAO.fragment");

    RHI::GraphicsPipelineDesc desc{};
    desc.vertexShader = s_SSAO.Vertex;
    desc.fragmentShader = s_SSAO.Fragment;
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0] = RHI::TextureFormat::R8_UNORM;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.blendMode = RHI::BlendMode::None;
    desc.cullMode = RHI::CullMode::None;
    desc.pushConstantSize = pushConstantSize;
    desc.debugName = "SSAO";
    s_SSAO.Pipeline = device.CreateGraphicsPipeline(desc);

    s_SSAO.Loaded = s_SSAO.Vertex != nullptr && s_SSAO.Fragment != nullptr && s_SSAO.Pipeline != nullptr;
    return s_SSAO.Loaded;
}

i32 BindlessIndex(RHI::TextureHandle texture)
{
    return texture != nullptr ? static_cast<i32>(reinterpret_cast<RHI::RHITexture*>(texture)->GetTextureIndex()) : -1;
}

/// @brief Records one fullscreen SSAO draw with \p pc.
void Draw(RHI::FGContext& context, const SSAOPushConstants& pc)
{
    if (!EnsureLoaded() || pc.DepthTexture < 0)
        return;
    RHI::RHICommandBuffer& cmd = context.Cmd();
    cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
    cmd.SetScissor(0, 0, context.Width(), context.Height());
    cmd.BindPipeline(s_SSAO.Pipeline);
    cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
    cmd.Draw(3, 0);
}
} // namespace

RHI::FGHandle<RHI::FGTexture> SSAO::AddPasses(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> depth, u32 width,
                                              u32 height, const glm::mat4& projection, const SSAOSettings& settings)
{
    using namespace RHI;
    // No shader, no passes: an occlusion image nothing drew into would be
    // cleared to 0 and take all ambient light away with it.
    if (!EnsureLoaded())
        return {};
    // Half resolution: occlusion is soft, and the colour pass upsamples it.
    const u32 halfWidth = std::max((width + 1) / 2, 1u);
    const u32 halfHeight = std::max((height + 1) / 2, 1u);
    SSAOPushConstants base;
    base.Projection = projection;
    base.InverseProjection = glm::inverse(projection);
    base.Radius = std::max(settings.Radius, 0.01f);
    base.Bias = std::max(settings.Bias, 0.0f);
    base.Intensity = std::clamp(settings.Intensity, 0.0f, 4.0f);

    struct OcclusionData
    {
        FGHandle<FGTexture> Depth;
        FGHandle<FGTexture> Raw;
    };
    const OcclusionData& occlusion = graph.AddPass<OcclusionData>(
        "SSAO.Occlusion",
        [&](FGBuilder& builder, OcclusionData& d)
        {
            d.Depth = builder.Read(depth);
            d.Raw = builder.Write(builder.Create("SSAO.Raw", {halfWidth, halfHeight, TextureFormat::R8_UNORM}),
                                  FGWrite::RenderTarget);
        },
        [base](const OcclusionData& d, FGContext& context)
        {
            SSAOPushConstants pc = base;
            pc.DepthTexture = BindlessIndex(context.Get(d.Depth));
            pc.Mode = 0;
            Draw(context, pc);
        });

    struct BlurData
    {
        FGHandle<FGTexture> Depth;
        FGHandle<FGTexture> Raw;
        FGHandle<FGTexture> Result;
    };
    const BlurData& blur = graph.AddPass<BlurData>(
        "SSAO.Blur",
        [&](FGBuilder& builder, BlurData& d)
        {
            d.Depth = builder.Read(depth);
            d.Raw = builder.Read(occlusion.Raw);
            d.Result = builder.Write(builder.Create("SSAO.Result", {halfWidth, halfHeight, TextureFormat::R8_UNORM}),
                                     FGWrite::RenderTarget);
        },
        [base](const BlurData& d, FGContext& context)
        {
            SSAOPushConstants pc = base;
            pc.DepthTexture = BindlessIndex(context.Get(d.Depth));
            pc.SourceTexture = BindlessIndex(context.Get(d.Raw));
            pc.Mode = 1;
            Draw(context, pc);
        });
    return blur.Result;
}

void SSAO::Shutdown()
{
    DestroyResources();
}

} // namespace UHE
