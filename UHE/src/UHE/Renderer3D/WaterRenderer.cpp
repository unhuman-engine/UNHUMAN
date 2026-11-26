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
    glm::vec2 Size{0.0f};
    i32 EntityID = -1; ///< an integer: a float loses entt handles above 2^24
    f32 Padding = 0.0f;
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
    bool Reported = false; ///< a failure was logged; again only after the next reload
    bool ListenerRegistered = false;
    ShaderCache::Handle Handle = 0;
    RHI::ShaderHandle Vertex = nullptr;
    RHI::ShaderHandle Fragment = nullptr;
    RHI::ShaderHandle FragmentCopy = nullptr; ///< no_entity_id variant: the copy pass has one target
    RHI::PipelineHandle Pipeline = nullptr;   ///< surfaces: HDR colour + entity ID
    RHI::PipelineHandle CopyPipeline = nullptr;
    WaterRenderer::Frame Current;
    bool Active = false;
};

WaterData s_Water;

void DestroyResources()
{
    auto& device = Renderer::GetDevice();
    if (s_Water.Pipeline != nullptr)
        device.DestroyGraphicsPipeline(s_Water.Pipeline);
    if (s_Water.CopyPipeline != nullptr)
        device.DestroyGraphicsPipeline(s_Water.CopyPipeline);
    if (s_Water.FragmentCopy != nullptr)
        device.DestroyShader(s_Water.FragmentCopy);
    s_Water.CopyPipeline = nullptr;
    s_Water.FragmentCopy = nullptr;
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
    // Both loaded before either is fetched: a Load can grow the library and
    // move what an earlier Get pointed at.
    s_Water.Handle = library.Load(path.string());
    const ShaderCache::Handle copyHandle = library.Load(path.string(), "no_entity_id");
    if (!s_Water.ListenerRegistered)
    {
        s_Water.ListenerRegistered = true;
        // 0 is ShaderCache::ReloadAll; the copy variant reloads with the main one.
        ShaderCache::Get().AddReloadListener(
            [copyHandle](ShaderCache::Handle handle)
            {
                if (handle != 0 && handle != s_Water.Handle && handle != copyHandle)
                    return;
                s_Water.Reported = false;
                if (!s_Water.Loaded)
                    return;
                Renderer::GetDevice().WaitIdle();
                DestroyResources();
            });
    }
    const auto fail = [](const std::string& why)
    {
        if (!s_Water.Reported)
            UHE_CORE_ERROR("WaterRenderer: {0}; water will not draw", why);
        s_Water.Reported = true;
        return false;
    };
    // The pipelines declare and the draws push all 256 bytes: a device that
    // allows less (Vulkan only guarantees 128) cannot run them.
    auto& device = Renderer::GetDevice();
    if (device.GetMaxPushConstantsSize() < sizeof(WaterPushConstants))
        return fail("the device allows " + std::to_string(device.GetMaxPushConstantsSize()) +
                    " bytes of push constants, the water shader needs " + std::to_string(sizeof(WaterPushConstants)));
    const ShaderCache::Shader* shader = library.Get(s_Water.Handle);
    const ShaderCache::Shader* copyShader = library.Get(copyHandle);
    if (shader == nullptr || copyShader == nullptr)
        return fail("Water.slang failed to load");
    const u32 pushConstantSize = ReflectedPushConstantSize(*shader);
    if (pushConstantSize != sizeof(WaterPushConstants) || ReflectedPushConstantSize(*copyShader) != pushConstantSize)
        return fail("Water.slang push constants are " + std::to_string(pushConstantSize) +
                    " bytes, the host struct is " + std::to_string(sizeof(WaterPushConstants)));

    s_Water.Vertex = CreateStageShader(device, *shader, RHI::ShaderStage::Vertex, "Water.vertex");
    s_Water.Fragment = CreateStageShader(device, *shader, RHI::ShaderStage::Fragment, "Water.fragment");
    s_Water.FragmentCopy =
        CreateStageShader(device, *copyShader, RHI::ShaderStage::Fragment, "Water.fragment.noEntityId");
    RHI::GraphicsPipelineDesc desc{};
    desc.vertexShader = s_Water.Vertex;
    desc.fragmentShader = s_Water.Fragment;
    desc.colorAttachmentCount = 2;
    desc.colorFormats[0] = RHI::TextureFormat::RGBA16F;
    desc.colorFormats[1] = RHI::TextureFormat::R32_SINT;
    // No depth attachment: the opaque depth is a texture the shader tests against.
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.blendMode = RHI::BlendMode::None;
    desc.cullMode = RHI::CullMode::None; // seen from below too
    desc.pushConstantSize = pushConstantSize;
    desc.debugName = "Water";
    s_Water.Pipeline = device.CreateGraphicsPipeline(desc);
    desc.fragmentShader = s_Water.FragmentCopy;
    desc.colorAttachmentCount = 1;
    desc.debugName = "Water.Copy";
    s_Water.CopyPipeline = device.CreateGraphicsPipeline(desc);

    s_Water.Loaded = s_Water.Vertex != nullptr && s_Water.Fragment != nullptr && s_Water.Pipeline != nullptr &&
                     s_Water.CopyPipeline != nullptr;
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

void WaterRenderer::CopyOpaque(i32 sceneColor)
{
    if (!EnsureLoaded() || sceneColor < 0)
        return;
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();
    cmd.BindPipeline(s_Water.CopyPipeline);
    WaterPushConstants pc;
    pc.SceneColor = sceneColor;
    pc.SceneDepth = sceneColor; // unused by the copy; any valid slot
    pc.WaveDir.w = 0.0f;
    cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
    cmd.Draw(3, 0);
}

void WaterRenderer::Begin(const Frame& frame)
{
    s_Water.Current = frame;
    s_Water.Active = EnsureLoaded() && frame.SceneColor >= 0 && frame.SceneDepth >= 0;
}

void WaterRenderer::Submit(const glm::mat4& transform, const WaterComponent& water, int entityID)
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
    pc.Size = glm::vec2(std::max(water.Size.x, 0.01f), std::max(water.Size.y, 0.01f));
    pc.EntityID = entityID;

    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();
    cmd.BindPipeline(s_Water.Pipeline);
    cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(pc), 0);
    cmd.Draw(static_cast<u32>(cells * cells * 6), 0);
}

void WaterRenderer::End()
{
    s_Water.Active = false;
}

bool WaterRenderer::IsAvailable()
{
    return EnsureLoaded();
}

void WaterRenderer::Shutdown()
{
    DestroyResources();
}

} // namespace UHE
