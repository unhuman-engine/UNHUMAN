#include "uhepch.h"
#include "UHE/Renderer3D/WaterRenderer.h"
#include <filesystem>
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer/ShaderResources.h"
#include "UHE/Renderer3D/Renderer3D.h"
#include "UHE/Scene/Components.h"

namespace UHE
{

namespace
{
/// Must match `PushConstants` in Water.slang; checked against reflection.
struct WaterPushConstants
{
    glm::mat4 ViewProj{1.0f};
    glm::mat4 Model{1.0f};
    glm::vec4 CameraPos{0.0f}; ///< w: time
    glm::vec4 Shallow{0.0f};   ///< a: clarity
    glm::vec4 Deep{0.0f};      ///< a: foam
    glm::vec4 Wave{0.0f};      ///< height, length, speed, choppiness
    glm::vec4 WaveDir{0.0f};   ///< xy: direction, z: cells, w: mode
    glm::vec4 Size{0.0f};
    glm::vec4 Projection{0.0f};
    i32 FrameData = -1;
    i32 LightBuffer = -1;
    i32 SceneColor = -1;
    i32 SceneDepth = -1;
};
static_assert(sizeof(WaterPushConstants) == 256, "WaterPushConstants must match Water.slang");

/// Upper bound on grid cells per side: 512^2 cells is 1.5M vertices for one surface.
constexpr int kMaxCells = 512;

struct WaterData
{
    bool Loaded = false;
    bool ListenerRegistered = false;
    ShaderCache::Handle Handle = 0;
    RHI::ShaderHandle Vertex = nullptr;
    RHI::ShaderHandle Fragment = nullptr;
    RHI::PipelineHandle Pipeline = nullptr;
    WaterRenderer::Frame Current;
    bool Active = false;
};

WaterData s_Water;

void DestroyResources()
{
    auto& device = Renderer::GetDevice();
    if (s_Water.Pipeline != nullptr)
        device.DestroyGraphicsPipeline(s_Water.Pipeline);
    if (s_Water.Vertex != nullptr)
        device.DestroyShader(s_Water.Vertex);
    if (s_Water.Fragment != nullptr)
        device.DestroyShader(s_Water.Fragment);
    s_Water.Pipeline = nullptr;
    s_Water.Vertex = nullptr;
    s_Water.Fragment = nullptr;
    s_Water.Loaded = false;
}

/// @brief Loads Water.slang and builds the pipeline on first use. @return false when unavailable.
bool EnsureLoaded()
{
    if (s_Water.Loaded)
        return true;
    ShaderCache& library = ShaderCache::Get();
    const std::filesystem::path path = FileSystem::Get().GetRootPath() / "assets/shaders/Water.slang";
    s_Water.Handle = library.Load(path.string());
    const ShaderCache::Shader* shader = library.Get(s_Water.Handle);
    if (shader == nullptr)
    {
        UHE_CORE_ERROR("WaterRenderer: Water.slang failed to load; water will not draw");
        return false;
    }

    auto& device = Renderer::GetDevice();
    s_Water.Vertex = CreateStageShader(device, *shader, RHI::ShaderStage::Vertex, "Water.vertex");
    s_Water.Fragment = CreateStageShader(device, *shader, RHI::ShaderStage::Fragment, "Water.fragment");
    const u32 pushConstantSize = ReflectedPushConstantSize(*shader);
    if (pushConstantSize != sizeof(WaterPushConstants))
        UHE_CORE_ERROR("WaterRenderer: Water.slang push constants are {0} bytes, the host struct is {1}",
                       pushConstantSize, sizeof(WaterPushConstants));

    RHI::GraphicsPipelineDesc desc{};
    desc.vertexShader = s_Water.Vertex;
    desc.fragmentShader = s_Water.Fragment;
    desc.colorAttachmentCount = 1;
    desc.colorFormats[0] = RHI::TextureFormat::RGBA16F;
    // No depth attachment: the opaque depth is a texture the shader tests against.
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.blendMode = RHI::BlendMode::None;
    desc.cullMode = RHI::CullMode::None; // seen from below too
    desc.pushConstantSize = pushConstantSize;
    desc.debugName = "Water";
    s_Water.Pipeline = device.CreateGraphicsPipeline(desc);

    if (!s_Water.ListenerRegistered)
    {
        s_Water.ListenerRegistered = true;
        ShaderCache::Get().AddReloadListener(
            [](ShaderCache::Handle handle)
            {
                if (handle != s_Water.Handle || !s_Water.Loaded)
                    return;
                Renderer::GetDevice().WaitIdle();
                DestroyResources();
            });
    }
    s_Water.Loaded = s_Water.Vertex != nullptr && s_Water.Fragment != nullptr && s_Water.Pipeline != nullptr;
    return s_Water.Loaded;
}

glm::vec3 SrgbToLinear(const glm::vec3& c)
{
    const auto channel = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return {channel(c.r), channel(c.g), channel(c.b)};
}

WaterPushConstants BasePushConstants()
{
    const WaterRenderer::Frame& frame = s_Water.Current;
    WaterPushConstants pc;
    pc.ViewProj = frame.ViewProjection;
    pc.CameraPos = glm::vec4(frame.CameraPosition, frame.Time);
    pc.Projection =
        glm::vec4(frame.Projection[2][2], frame.Projection[3][2], frame.Projection[2][3], frame.Projection[3][3]);
    pc.FrameData = Renderer3D::GetFrameDataBufferIndex();
    pc.LightBuffer = Renderer3D::GetLightBufferIndex();
    pc.SceneColor = frame.SceneColor;
    pc.SceneDepth = frame.SceneDepth;
    return pc;
}
} // namespace

void WaterRenderer::Begin(const Frame& frame)
{
    s_Water.Current = frame;
    s_Water.Active = EnsureLoaded() && frame.SceneColor >= 0 && frame.SceneDepth >= 0;
    if (!s_Water.Active)
        return;
    // The target starts as a copy of the opaque scene; surfaces draw over it.
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();
    cmd.BindPipeline(s_Water.Pipeline);
    WaterPushConstants pc = BasePushConstants();
    pc.WaveDir.w = 0.0f;
    cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
    cmd.Draw(3, 0);
}

void WaterRenderer::Submit(const glm::mat4& transform, const WaterComponent& water)
{
    if (!s_Water.Active)
        return;
    const int cells = std::clamp(water.Resolution, 1, kMaxCells);
    glm::vec2 direction = s_Water.Current.WindDirection;
    if (!water.FollowWind || glm::length(direction) < 1e-4f)
    {
        const float radians = glm::radians(water.Direction);
        direction = glm::vec2(std::cos(radians), std::sin(radians));
    }

    WaterPushConstants pc = BasePushConstants();
    pc.Model = transform;
    pc.Shallow = glm::vec4(SrgbToLinear(water.ShallowColor), std::max(water.Clarity, 0.05f));
    pc.Deep = glm::vec4(SrgbToLinear(water.DeepColor), std::clamp(water.Foam, 0.0f, 1.0f));
    pc.Wave = glm::vec4(std::max(water.WaveHeight, 0.0f), std::max(water.WaveLength, 0.1f),
                        std::max(water.WaveSpeed, 0.0f), std::clamp(water.Choppiness, 0.0f, 1.0f));
    pc.WaveDir = glm::vec4(glm::normalize(direction), static_cast<float>(cells), 1.0f);
    pc.Size = glm::vec4(std::max(water.Size.x, 0.01f), std::max(water.Size.y, 0.01f), 0.0f, 0.0f);

    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();
    cmd.BindPipeline(s_Water.Pipeline);
    cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
    cmd.Draw(static_cast<u32>(cells * cells * 6), 0);
}

void WaterRenderer::End()
{
    s_Water.Active = false;
}

void WaterRenderer::Shutdown()
{
    DestroyResources();
}

} // namespace UHE
