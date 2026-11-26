#include "uhepch.h"
#include "UHE/Renderer3D/SkyRenderer.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer/ShaderResources.h"
#include "UHE/Renderer/Texture.h"

namespace UHE
{

namespace
{
/// Must match `PushConstants` in Sky.slang; checked against reflection.
struct SkyPushConstants
{
    glm::mat4 InverseViewProjection{1.0f};
    glm::vec4 CameraPosition{0.0f};
    glm::vec4 SunDirection{0.0f};
    glm::vec4 SunColor{0.0f};
    glm::vec4 Zenith{0.0f};
    glm::vec4 Horizon{0.0f};
    glm::vec4 Ground{0.0f};
    i32 DepthTexture = -1;
    i32 Mode = 1;
    i32 ImageTexture = -1;
    f32 ImageRotation = 0.0f;
};
static_assert(sizeof(SkyPushConstants) == 176, "SkyPushConstants must match Sky.slang");

struct SkyData
{
    bool Loaded = false;
    bool Reported = false; ///< a failure was logged; again only after the next reload
    bool ListenerRegistered = false;
    ShaderCache::Handle Handle = 0;
    RHI::ShaderHandle Vertex = nullptr;
    RHI::ShaderHandle Fragment = nullptr;
    RHI::PipelineHandle Pipeline = nullptr;

    std::string ImageKey; ///< the resolved file (and its time) Image was loaded from, or failed to load from
    Ref<Texture2D> Image;
};

SkyData s_Sky;

void DestroyPipeline()
{
    auto& device = Renderer::GetDevice();
    if (s_Sky.Pipeline != nullptr)
        device.DestroyGraphicsPipeline(s_Sky.Pipeline);
    if (s_Sky.Vertex != nullptr)
        device.DestroyShader(s_Sky.Vertex);
    if (s_Sky.Fragment != nullptr)
        device.DestroyShader(s_Sky.Fragment);
    s_Sky.Pipeline = nullptr;
    s_Sky.Vertex = nullptr;
    s_Sky.Fragment = nullptr;
    s_Sky.Loaded = false;
}

bool EnsureLoaded()
{
    if (s_Sky.Loaded)
        return true;
    if (s_Sky.Reported)
        return false; // retried after the next reload
    ShaderCache& library = ShaderCache::Get();
    const std::filesystem::path path = FileSystem::Get().GetRootPath() / "assets/shaders/Sky.slang";
    s_Sky.Handle = library.Load(path.string());
    if (!s_Sky.ListenerRegistered)
    {
        s_Sky.ListenerRegistered = true;
        // Rebuilt lazily on the next use; queued frames still use the old objects. 0 is ShaderCache::ReloadAll.
        ShaderCache::Get().AddReloadListener(
            [](ShaderCache::Handle handle)
            {
                if (handle != 0 && handle != s_Sky.Handle)
                    return;
                s_Sky.Reported = false;
                if (!s_Sky.Loaded)
                    return;
                Renderer::GetDevice().WaitIdle();
                DestroyPipeline();
            });
    }
    const ShaderCache::Shader* shader = library.Get(s_Sky.Handle);
    if (shader == nullptr)
    {
        UHE_CORE_ERROR("Sky: Sky.slang failed to load; the sky is the clear colour");
        s_Sky.Reported = true;
        return false;
    }
    // Draw pushes the whole host struct; a smaller declared range would be overrun.
    const u32 pushConstantSize = ReflectedPushConstantSize(*shader);
    if (pushConstantSize != sizeof(SkyPushConstants))
    {
        UHE_CORE_ERROR("Sky: Sky.slang push constants are {0} bytes, the host struct is {1}; the sky is the clear "
                       "colour",
                       pushConstantSize, sizeof(SkyPushConstants));
        s_Sky.Reported = true;
        return false;
    }

    auto& device = Renderer::GetDevice();
    s_Sky.Vertex = CreateStageShader(device, *shader, RHI::ShaderStage::Vertex, "Sky.vertex");
    s_Sky.Fragment = CreateStageShader(device, *shader, RHI::ShaderStage::Fragment, "Sky.fragment");

    RHI::GraphicsPipelineDesc desc{};
    desc.vertexShader = s_Sky.Vertex;
    desc.fragmentShader = s_Sky.Fragment;
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0] = RHI::TextureFormat::RGBA16F;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.blendMode = RHI::BlendMode::None;
    desc.cullMode = RHI::CullMode::None;
    desc.pushConstantSize = pushConstantSize;
    desc.debugName = "Sky";
    s_Sky.Pipeline = device.CreateGraphicsPipeline(desc);

    s_Sky.Loaded = s_Sky.Vertex != nullptr && s_Sky.Fragment != nullptr && s_Sky.Pipeline != nullptr;
    return s_Sky.Loaded;
}

float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

glm::vec3 SrgbToLinear(const glm::vec3& c)
{
    return {SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b)};
}
} // namespace

bool SkyRenderer::Draws(const SkySettings& settings)
{
    return settings.Mode == SkySettings::SkyMode::Procedural || settings.Mode == SkySettings::SkyMode::Image;
}

void SkyRenderer::PrepareImage(const SkySettings& settings)
{
    const std::string path = settings.Mode == SkySettings::SkyMode::Image ? settings.ImagePath : std::string();
    // Keyed by the file the path resolves to, and its time: the same relative
    // path in another project (or a repainted image) is another image.
    std::string key;
    std::filesystem::path resolved;
    if (!path.empty())
    {
        resolved = ResolveAssetPath(path);
        std::error_code ec;
        const auto written = std::filesystem::last_write_time(resolved, ec);
        key = resolved.string() + "|" +
              (ec ? std::string("missing") : std::to_string(written.time_since_epoch().count()));
    }
    if (key == s_Sky.ImageKey)
        return;
    s_Sky.ImageKey = key;
    // The previous image may still be read by a frame in flight; the texture
    // defers its own destruction until that frame is done.
    s_Sky.Image.reset();
    if (path.empty())
        return;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(resolved, ec))
    {
        UHE_CORE_ERROR("Sky: the image '{0}' was not found; the sky shows its horizon colour", path);
        return;
    }
    RHI::SamplerDesc sampler;
    sampler.wrapS = RHI::SamplerDesc::Wrap::Repeat;
    sampler.wrapT = RHI::SamplerDesc::Wrap::ClampToEdge;
    s_Sky.Image = Texture2D::Create(resolved.string(), sampler);
    UHE_CORE_INFO("Sky: '{0}' ({1}x{2})", path, s_Sky.Image->GetWidth(), s_Sky.Image->GetHeight());
}

void SkyRenderer::Draw(RHI::FGContext& context, const SkySettings& settings, const Frame& frame, i32 depthTexture)
{
    if (!Draws(settings) || depthTexture < 0 || !EnsureLoaded())
        return;

    SkyPushConstants pc;
    pc.InverseViewProjection = frame.InverseViewProjection;
    pc.CameraPosition = glm::vec4(frame.CameraPosition, 1.0f);
    pc.SunDirection = glm::vec4(frame.ToSun, frame.HasSun ? 1.0f : 0.0f);
    pc.SunColor = glm::vec4(frame.SunColor, 1.0f);
    const float radius = glm::radians(std::clamp(settings.SunSize, 0.05f, 20.0f) * 0.5f);
    pc.Zenith = glm::vec4(SrgbToLinear(settings.ZenithColor), std::max(settings.Intensity, 0.0f));
    pc.Horizon = glm::vec4(SrgbToLinear(settings.HorizonColor), std::cos(radius));
    pc.Ground = glm::vec4(SrgbToLinear(settings.GroundColor), std::max(settings.SunBrightness, 0.0f));
    pc.DepthTexture = depthTexture;
    pc.Mode = static_cast<i32>(settings.Mode);
    pc.ImageTexture = s_Sky.Image ? static_cast<i32>(s_Sky.Image->GetTextureIndex()) : -1;
    pc.ImageRotation = glm::radians(settings.ImageRotation);

    RHI::RHICommandBuffer& cmd = context.Cmd();
    cmd.BindPipeline(s_Sky.Pipeline);
    cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
    cmd.Draw(3, 0);
}

glm::vec3 SkyRenderer::ZenithRadiance(const SkySettings& settings)
{
    return SrgbToLinear(settings.ZenithColor) * std::max(settings.Intensity, 0.0f);
}

glm::vec3 SkyRenderer::HorizonRadiance(const SkySettings& settings)
{
    return SrgbToLinear(settings.HorizonColor) * std::max(settings.Intensity, 0.0f);
}

float SkyRenderer::Daylight(float sinElevation)
{
    const float t = std::clamp((sinElevation + 0.28f) / 0.40f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void SkyRenderer::Shutdown()
{
    DestroyPipeline();
    s_Sky.Image.reset();
    s_Sky.ImageKey.clear();
    s_Sky.Reported = false;
}

} // namespace UHE
