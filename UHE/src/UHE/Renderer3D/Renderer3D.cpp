#include "uhepch.h"
#include "Renderer3D.h"
#include <array>
#include <glm/gtc/matrix_transform.hpp>
#include <map>
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer/ShaderResources.h"
#include "UHE/Renderer3D/MaterialGPU.h"

namespace UHE
{

namespace
{

// Capacity of the per-frame material region. Every model uploaded this frame
// takes GetMaterials().size() slots; a scene that exceeds this draws its
// overflow with the default material rather than reading out of bounds. Slot 0
// is reserved for that default and is never handed to a model.
constexpr uint32_t kMaxMaterialsPerFrame = 2048;
constexpr uint32_t kMaxLightsPerFrame = 1024;
constexpr uint32_t kMaxBonesPerFrame = 4096;
// The device keeps this many frames in flight (VulkanDevice::MAX_FRAMES_IN_FLIGHT,
// the same count Renderer2D double-buffers its vertex buffers for).
constexpr uint32_t kFramesInFlight = 2;

} // namespace

struct Renderer3DData
{
    RHI::ShaderHandle VertexShader;
    RHI::ShaderHandle FragmentShader;
    /// The same source with `UHE_NO_ENTITY_ID`: for the pass that renders into
    /// one attachment and so has no picking target to write.
    RHI::ShaderHandle FragmentShaderNoId;
    RHI::PipelineHandle ModelPipeline;
    // Same shaders and layout, cull mode None. Cull mode is baked into the
    // pipeline, so a doubleSided material cannot be drawn with ModelPipeline -
    // it needs this variant bound instead.
    RHI::PipelineHandle ModelPipelineDoubleSided;
    // Wireframe view: the same shaders, edges only, no culling (the back
    // edges are part of the shape).
    RHI::PipelineHandle ModelPipelineWireframe;

    RHI::ShaderHandle GridVertexShader;
    RHI::ShaderHandle GridFragmentShader;
    RHI::ShaderHandle GridFragmentShaderNoId; ///< for single-attachment passes
    RHI::PipelineHandle GridPipeline;

    // The editor draws the scene into its own framebuffer, which is a colour
    // target plus the entity-id target; the game path draws it straight into
    // the swapchain, which is one attachment in the swapchain's own format. A
    // pipeline has to match the pass it is bound in, so those passes get their
    // own variants rather than the two-attachment RGBA8 one above.
    // One set per colour format, built on first use from the descs below: the
    // swapchain's format for the game path, RGBA16F for an HDR scene target.
    struct SingleTargetPipelines
    {
        RHI::PipelineHandle Model = nullptr;
        RHI::PipelineHandle ModelDoubleSided = nullptr;
        RHI::PipelineHandle ModelWireframe = nullptr;
        RHI::PipelineHandle Grid = nullptr;
    };
    std::map<RHI::TextureFormat, SingleTargetPipelines> SingleTargets;
    RHI::GraphicsPipelineDesc SingleTargetModelDesc{};
    RHI::GraphicsPipelineDesc SingleTargetGridDesc{};
    bool ShaderResourcesReady = false;
    /// Undefined: the editor's two-attachment set; otherwise a single-target format.
    RHI::TextureFormat TargetFormat = RHI::TextureFormat::Undefined;

    /// Push constant bytes the pipelines were built with, read from the shader's
    /// reflection. Compared against the host struct before every upload, so a
    /// shader and a struct that drift apart are reported instead of writing past
    /// the block.
    u32 PushConstantSize = 0;

    glm::mat4 ViewProjection;
    glm::vec3 CameraPosition;
    std::vector<RD3d::LightData> CurrentLights;
    Ref<Texture2D> WhiteTexture;

    // The light, bone and material buffers are host-visible and written with a
    // plain memcpy while the GPU may still be reading the previous frame's
    // copy. One set per frame in flight; BeginScene selects this frame's set
    // into the fields below, which every other function uses.
    struct FrameBuffers
    {
        RHI::BufferHandle Light = nullptr;
        uint32_t LightIndex = 0;
        RHI::BufferHandle Bone = nullptr;
        uint32_t BoneIndex = 0;
        RHI::BufferHandle Material = nullptr;
        uint32_t MaterialIndex = 0;
    };
    std::array<FrameBuffers, kFramesInFlight> Frames{};

    RHI::BufferHandle LightStorageBufferHandle = nullptr;
    uint32_t LightStorageBufferIndex = 0;

    RHI::BufferHandle BoneStorageBufferHandle = nullptr;
    uint32_t BoneStorageBufferIndex = 0;
    uint32_t BoneBufferOffset = 0; // In number of matrices

    // Per-material parameters (MaterialGPU.h), indexed bindlessly from push
    // constants. Like the bone buffer this is a per-frame region: the cursor
    // restarts each BeginScene and every visible model re-uploads its
    // materials. A persistent residency cache would avoid the re-upload, but
    // needs lifetime hooks on Model destruction that do not exist yet.
    RHI::BufferHandle MaterialStorageBufferHandle = nullptr;
    uint32_t MaterialStorageBufferIndex = 0;
    uint32_t MaterialBufferOffset = 0;

    bool EnableLighting = true;
    Renderer3D::ViewMode Mode = Renderer3D::ViewMode::Lit;
};

static Renderer3DData s_Data3D;

/// @brief The single-attachment pipelines for \p format, built the first time it is asked for.
/// @return nullptr for TextureFormat::Undefined (the editor's set) or before the shaders load.
static const Renderer3DData::SingleTargetPipelines* SingleTargetFor(RHI::TextureFormat format)
{
    if (format == RHI::TextureFormat::Undefined || !s_Data3D.ShaderResourcesReady)
        return nullptr;
    const auto it = s_Data3D.SingleTargets.find(format);
    if (it != s_Data3D.SingleTargets.end())
        return &it->second;

    auto& device = Renderer::GetDevice();
    Renderer3DData::SingleTargetPipelines set;
    RHI::GraphicsPipelineDesc model = s_Data3D.SingleTargetModelDesc;
    model.colorFormats[0] = format;
    model.debugName = "ModelPipeline.SingleTarget";
    set.Model = device.CreateGraphicsPipeline(model);
    RHI::GraphicsPipelineDesc doubleSided = model;
    doubleSided.cullMode = RHI::CullMode::None;
    doubleSided.debugName = "ModelPipeline.SingleTarget.DoubleSided";
    set.ModelDoubleSided = device.CreateGraphicsPipeline(doubleSided);
    RHI::GraphicsPipelineDesc wireframe = doubleSided;
    wireframe.wireframe = true;
    wireframe.debugName = "ModelPipeline.SingleTarget.Wireframe";
    set.ModelWireframe = device.CreateGraphicsPipeline(wireframe);
    RHI::GraphicsPipelineDesc grid = s_Data3D.SingleTargetGridDesc;
    grid.colorFormats[0] = format;
    grid.debugName = "GridPipeline.SingleTarget";
    set.Grid = device.CreateGraphicsPipeline(grid);
    return &s_Data3D.SingleTargets.emplace(format, set).first->second;
}

void Renderer3D::Init()
{
    auto& device = Renderer::GetDevice();

    // Shaders and the pipelines built from them: created together, and re-created
    // together when a shader changes on disk.
    CreateShaderResources();

    // A shader edit invalidates the bytes every pipeline above was built from, so
    // the pipelines have to be rebuilt with them. Registered once: Init runs once
    // per engine start and the listener outlives it.
    static bool listenerRegistered = false;
    if (!listenerRegistered)
    {
        listenerRegistered = true;
        ShaderCache::Get().AddReloadListener([](ShaderCache::Handle) { Renderer3D::ReloadShaders(); });
    }

    s_Data3D.WhiteTexture = Texture2D::Create(1, 1);

    for (auto& frame : s_Data3D.Frames)
    {
        RHI::BufferDesc desc;
        desc.size = sizeof(RD3d::LightData) * kMaxLightsPerFrame;
        desc.usage = RHI::BufferUsage::Storage;
        desc.hostVisible = true;
        frame.Light = device.CreateBuffer(desc);
        frame.LightIndex = device.GetBufferBindlessIndex(frame.Light);

        RHI::BufferDesc boneBufferDesc{};
        boneBufferDesc.size = sizeof(glm::mat4) * kMaxBonesPerFrame;
        boneBufferDesc.usage = RHI::BufferUsage::Storage;
        boneBufferDesc.hostVisible = true;
        frame.Bone = device.CreateBuffer(boneBufferDesc);
        frame.BoneIndex = device.GetBufferBindlessIndex(frame.Bone);

        RHI::BufferDesc materialBufferDesc{};
        materialBufferDesc.size = sizeof(RD3d::MaterialGPU) * kMaxMaterialsPerFrame;
        materialBufferDesc.usage = RHI::BufferUsage::Storage;
        materialBufferDesc.hostVisible = true;
        frame.Material = device.CreateBuffer(materialBufferDesc);
        // The material array lives in its own bindless namespace (shader
        // binding 2), separate from the light/bone arrays of binding 0.
        frame.MaterialIndex = device.GetMaterialBufferBindlessIndex(frame.Material);
    }
    SelectFrameBuffers();
    s_Data3D.MaterialBufferOffset = 1; // Slot 0 is the default material, written each BeginScene.
}

void Renderer3D::Shutdown()
{
    auto& device = Renderer::GetDevice();
    DestroyShaderResources();
    for (auto& frame : s_Data3D.Frames)
    {
        device.DestroyBuffer(frame.Light);
        device.DestroyBuffer(frame.Bone);
        device.DestroyBuffer(frame.Material);
        frame = {};
    }
    s_Data3D.LightStorageBufferHandle = nullptr;
    s_Data3D.BoneStorageBufferHandle = nullptr;
    s_Data3D.MaterialStorageBufferHandle = nullptr;

    s_Data3D.WhiteTexture.reset();
}

void Renderer3D::CreateShaderResources()
{
    auto& device = Renderer::GetDevice();
    ShaderCache& library = ShaderCache::Get();

    const std::filesystem::path shaderDir = FileSystem::Get().GetRootPath() / "assets/shaders";

    // "no_entity_id" is Basic3D.slang with UHE_NO_ENTITY_ID: the swapchain pass has
    // a single colour attachment, and a fragment output with no matching
    // attachment is invalid.
    const ShaderCache::Handle modelHandle = library.Load((shaderDir / "Basic3D.slang").string());
    const ShaderCache::Handle modelNoIdHandle = library.Load((shaderDir / "Basic3D.slang").string(), "no_entity_id");
    const ShaderCache::Handle gridHandle = library.Load((shaderDir / "Grid.slang").string());
    const ShaderCache::Handle gridNoIdHandle = library.Load((shaderDir / "Grid.slang").string(), "no_entity_id");

    const ShaderCache::Shader* model = library.Get(modelHandle);
    const ShaderCache::Shader* modelNoId = library.Get(modelNoIdHandle);
    const ShaderCache::Shader* grid = library.Get(gridHandle);
    const ShaderCache::Shader* gridNoId = library.Get(gridNoIdHandle);
    if (model == nullptr || modelNoId == nullptr || grid == nullptr || gridNoId == nullptr)
    {
        UHE_CORE_ERROR("Renderer3D: a scene shader failed to load; the scene will not draw");
        return;
    }

    s_Data3D.VertexShader = CreateStageShader(device, *model, RHI::ShaderStage::Vertex, "Basic3D.vertex");
    s_Data3D.FragmentShader = CreateStageShader(device, *model, RHI::ShaderStage::Fragment, "Basic3D.fragment");
    s_Data3D.FragmentShaderNoId =
        CreateStageShader(device, *modelNoId, RHI::ShaderStage::Fragment, "Basic3D.fragment.noEntityId");
    s_Data3D.GridVertexShader = CreateStageShader(device, *grid, RHI::ShaderStage::Vertex, "Grid.vertex");
    s_Data3D.GridFragmentShader = CreateStageShader(device, *grid, RHI::ShaderStage::Fragment, "Grid.fragment");
    s_Data3D.GridFragmentShaderNoId =
        CreateStageShader(device, *gridNoId, RHI::ShaderStage::Fragment, "Grid.fragment.noEntityId");

    RHI::GraphicsPipelineDesc pipeDesc{};
    pipeDesc.vertexShader = s_Data3D.VertexShader;
    pipeDesc.fragmentShader = s_Data3D.FragmentShader;
    // Read out of the SPIR-V, not copied from the shader by hand: the vertex
    // layout is the module's own input list in location order, and the push
    // constant range is the block size the module declares.
    pipeDesc.vertexLayout = ReflectedVertexLayout(*model);
    pipeDesc.pushConstantSize = ReflectedPushConstantSize(*model);
    s_Data3D.PushConstantSize = pipeDesc.pushConstantSize;

    // Push constants only carry where things live (camera, bones, lights, and
    // the material's buffer location); the material's VALUES live in the
    // per-material storage buffer, because this block is at the 256-byte device
    // limit with no room for one more field.
    pipeDesc.blendMode = RHI::BlendMode::Alpha;
    pipeDesc.depthTest = true;
    pipeDesc.depthWrite = true;
    pipeDesc.cullMode = RHI::CullMode::Back;

    pipeDesc.colorAttachmentCount = 2;
    pipeDesc.colorFormats[0] = RHI::TextureFormat::RGBA8_SRGB;
    pipeDesc.colorFormats[1] = RHI::TextureFormat::R32_SINT;

    s_Data3D.ModelPipeline = device.CreateGraphicsPipeline(pipeDesc);

    // Double-sided variant: identical except culling, selected per material.
    RHI::GraphicsPipelineDesc doubleSidedDesc = pipeDesc;
    doubleSidedDesc.cullMode = RHI::CullMode::None;
    doubleSidedDesc.debugName = "ModelPipeline.DoubleSided";
    s_Data3D.ModelPipelineDoubleSided = device.CreateGraphicsPipeline(doubleSidedDesc);

    RHI::GraphicsPipelineDesc wireframeDesc = doubleSidedDesc;
    wireframeDesc.wireframe = true;
    wireframeDesc.debugName = "ModelPipeline.Wireframe";
    s_Data3D.ModelPipelineWireframe = device.CreateGraphicsPipeline(wireframeDesc);

    RHI::GraphicsPipelineDesc gridPipeDesc{};
    gridPipeDesc.vertexShader = s_Data3D.GridVertexShader;
    gridPipeDesc.fragmentShader = s_Data3D.GridFragmentShader;
    // The grid generates its vertices from gl_VertexIndex, so the module declares
    // no vertex inputs and reflection returns an empty layout.
    gridPipeDesc.vertexLayout = ReflectedVertexLayout(*grid);
    gridPipeDesc.pushConstantSize = ReflectedPushConstantSize(*grid);
    gridPipeDesc.blendMode = RHI::BlendMode::Alpha;
    // Drawn after the scene: tested against it, so geometry hides the grid,
    // but never written, so the translucent plane cannot hide what follows.
    gridPipeDesc.depthTest = true;
    gridPipeDesc.depthWrite = false;

    gridPipeDesc.colorAttachmentCount = 2;
    gridPipeDesc.colorFormats[0] = RHI::TextureFormat::RGBA8_SRGB;
    gridPipeDesc.colorFormats[1] = RHI::TextureFormat::R32_SINT;

    s_Data3D.GridPipeline = device.CreateGraphicsPipeline(gridPipeDesc);

    // Single-target variants: one colour attachment. The fragment shader is
    // the no_entity_id variant: an output with no matching attachment is invalid.
    s_Data3D.SingleTargetModelDesc = pipeDesc;
    s_Data3D.SingleTargetModelDesc.colorAttachmentCount = 1;
    s_Data3D.SingleTargetModelDesc.fragmentShader = s_Data3D.FragmentShaderNoId;
    s_Data3D.SingleTargetGridDesc = gridPipeDesc;
    s_Data3D.SingleTargetGridDesc.colorAttachmentCount = 1;
    s_Data3D.SingleTargetGridDesc.fragmentShader = s_Data3D.GridFragmentShaderNoId;
    s_Data3D.ShaderResourcesReady = true;
    // The swapchain set is built now rather than on first use, as before.
    (void)SingleTargetFor(device.GetSwapchainFormat());
}

void Renderer3D::DestroyShaderResources()
{
    auto& device = Renderer::GetDevice();

    device.DestroyGraphicsPipeline(s_Data3D.ModelPipeline);
    device.DestroyGraphicsPipeline(s_Data3D.ModelPipelineDoubleSided);
    device.DestroyGraphicsPipeline(s_Data3D.ModelPipelineWireframe);
    for (auto& [format, set] : s_Data3D.SingleTargets)
    {
        device.DestroyGraphicsPipeline(set.Model);
        device.DestroyGraphicsPipeline(set.ModelDoubleSided);
        device.DestroyGraphicsPipeline(set.ModelWireframe);
        device.DestroyGraphicsPipeline(set.Grid);
    }
    s_Data3D.SingleTargets.clear();
    s_Data3D.ShaderResourcesReady = false;
    device.DestroyGraphicsPipeline(s_Data3D.GridPipeline);
    device.DestroyShader(s_Data3D.VertexShader);
    device.DestroyShader(s_Data3D.FragmentShader);
    device.DestroyShader(s_Data3D.FragmentShaderNoId);
    device.DestroyShader(s_Data3D.GridVertexShader);
    device.DestroyShader(s_Data3D.GridFragmentShader);
    device.DestroyShader(s_Data3D.GridFragmentShaderNoId);

    // Nulled, not left dangling: a second destroy of the same handle would free
    // someone else's object.
    s_Data3D.ModelPipeline = nullptr;
    s_Data3D.ModelPipelineDoubleSided = nullptr;
    s_Data3D.ModelPipelineWireframe = nullptr;
    s_Data3D.GridPipeline = nullptr;
    s_Data3D.VertexShader = nullptr;
    s_Data3D.FragmentShader = nullptr;
    s_Data3D.FragmentShaderNoId = nullptr;
    s_Data3D.GridVertexShader = nullptr;
    s_Data3D.GridFragmentShader = nullptr;
    s_Data3D.GridFragmentShaderNoId = nullptr;
}

void Renderer3D::ReloadShaders()
{
    // Frames already queued reference the modules and pipelines being replaced.
    Renderer::GetDevice().WaitIdle();

    DestroyShaderResources();
    CreateShaderResources();
    UHE_CORE_INFO("Renderer3D: pipelines rebuilt from the reloaded shaders");
}

void Renderer3D::BeginScene(const EditorCamera& camera, const std::vector<RD3d::LightData>& lights)
{
    UHE_PROFILE_FUNCTION();
    s_Data3D.ViewProjection = camera.GetViewProjection();
    s_Data3D.CameraPosition = camera.GetPosition();
    SelectFrameBuffers();
    UploadLights(lights);
    s_Data3D.BoneBufferOffset = 0;

    RestartMaterialRegion();
}

void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform,
                            const std::vector<RD3d::LightData>& lights)
{
    UHE_PROFILE_FUNCTION();
    s_Data3D.ViewProjection = camera.GetProjection() * glm::inverse(transform);
    s_Data3D.CameraPosition = glm::vec3(transform[3]);
    SelectFrameBuffers();
    UploadLights(lights);
    s_Data3D.BoneBufferOffset = 0;

    RestartMaterialRegion();
}

void Renderer3D::SelectFrameBuffers()
{
    const auto& frame = s_Data3D.Frames[Renderer::GetDevice().GetCurrentFrameIndex() % kFramesInFlight];
    s_Data3D.LightStorageBufferHandle = frame.Light;
    s_Data3D.LightStorageBufferIndex = frame.LightIndex;
    s_Data3D.BoneStorageBufferHandle = frame.Bone;
    s_Data3D.BoneStorageBufferIndex = frame.BoneIndex;
    s_Data3D.MaterialStorageBufferHandle = frame.Material;
    s_Data3D.MaterialStorageBufferIndex = frame.MaterialIndex;
}

void Renderer3D::UploadLights(const std::vector<RD3d::LightData>& lights)
{
    s_Data3D.CurrentLights = lights;
    if (s_Data3D.CurrentLights.size() > kMaxLightsPerFrame)
    {
        // The buffer refuses an oversized upload outright, which would leave
        // every light stale; draw the first kMaxLightsPerFrame instead.
        UHE_CORE_WARN("Renderer3D: {0} lights, only the first {1} are drawn", s_Data3D.CurrentLights.size(),
                      kMaxLightsPerFrame);
        s_Data3D.CurrentLights.resize(kMaxLightsPerFrame);
    }
    if (!s_Data3D.CurrentLights.empty())
    {
        Renderer::GetDevice().GetCurrentCommandBuffer().UpdateBuffer(
            s_Data3D.LightStorageBufferHandle, s_Data3D.CurrentLights.data(),
            s_Data3D.CurrentLights.size() * sizeof(RD3d::LightData));
    }
}

void Renderer3D::RestartMaterialRegion()
{
    // Slot 0: the default material (glTF's "no material" white PBR surface).
    // Rewritten every frame so the host-visible buffer never has to be
    // initialised outside a command buffer.
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    RD3d::Material defaultMaterial;
    RD3d::MaterialGPU defaultGPU = RD3d::FillMaterialGPU(defaultMaterial);
    cmd.UpdateBuffer(s_Data3D.MaterialStorageBufferHandle, &defaultGPU, sizeof(defaultGPU), 0);

    s_Data3D.MaterialBufferOffset = 1;
}

void Renderer3D::EndScene() {}

void Renderer3D::SetSwapchainTarget(bool enabled)
{
    SetColorTarget(enabled ? Renderer::GetDevice().GetSwapchainFormat() : RHI::TextureFormat::Undefined);
}

void Renderer3D::SetColorTarget(RHI::TextureFormat format)
{
    s_Data3D.TargetFormat = format;
}

void Renderer3D::DrawGrid()
{
    DrawGrid(GridOptions{});
}

void Renderer3D::DrawGrid(const GridOptions& options)
{
    UHE_PROFILE_FUNCTION();
    if (options.Opacity <= 0.0f)
        return;
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    const auto* single = SingleTargetFor(s_Data3D.TargetFormat);
    cmd.BindPipeline(single != nullptr ? single->Grid : s_Data3D.GridPipeline);

    // MUST match PushConstants in Grid.slang.
    struct GridPushConstants
    {
        glm::mat4 viewProj;
        glm::mat4 inverseViewProj;
        glm::vec4 cameraPos; // xyz: eye
        glm::vec4 options;   // x: opacity, y: axes on (1) / off (0)
    } pc;
    static_assert(sizeof(GridPushConstants) == 160, "GridPushConstants must match Grid.slang");
    pc.viewProj = s_Data3D.ViewProjection;
    pc.inverseViewProj = glm::inverse(s_Data3D.ViewProjection);
    pc.cameraPos = glm::vec4(s_Data3D.CameraPosition, 1.0f);
    pc.options = glm::vec4(std::clamp(options.Opacity, 0.0f, 1.0f), options.Axes ? 1.0f : 0.0f, 0.0f, 0.0f);

    cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(GridPushConstants), 0);

    // Draw 6 vertices for the full-screen quad (generated by SV_VertexID)
    cmd.Draw(6, 0);
}

Renderer3D::BoneBinding Renderer3D::PrepareBoneBinding(const RD3d::Animator* animator)
{
    BoneBinding binding;
    if (animator && animator->HasAnimation())
    {
        const auto& matrices = animator->GetFinalBoneMatrices();
        if (!matrices.empty() && s_Data3D.BoneBufferOffset + matrices.size() > kMaxBonesPerFrame)
        {
            // Out of room this frame. The upload would be refused while the
            // binding still pointed past the buffer; draw this model unskinned.
            UHE_CORE_WARN("Renderer3D: bone buffer full ({0} matrices this frame), a skinned model draws in "
                          "bind pose",
                          kMaxBonesPerFrame);
        }
        else if (!matrices.empty())
        {
            uint64_t size = matrices.size() * sizeof(glm::mat4);
            auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();
            cmd.UpdateBuffer(s_Data3D.BoneStorageBufferHandle, matrices.data(), size,
                             s_Data3D.BoneBufferOffset * sizeof(glm::mat4));
            binding.BufferIndex = s_Data3D.BoneStorageBufferIndex;
            binding.Offset = s_Data3D.BoneBufferOffset;
            s_Data3D.BoneBufferOffset += matrices.size();
        }
    }
    return binding;
}

Renderer3D::MaterialBinding Renderer3D::PrepareMaterialBinding(const RD3d::Model& model)
{
    MaterialBinding binding;
    binding.BufferIndex = static_cast<int>(s_Data3D.MaterialStorageBufferIndex);
    binding.BaseIndex = 0;
    binding.Count = 1;
    const auto& materials = model.GetMaterials();
    if (materials.empty())
        return binding; // draws fall back to the default material (slot 0)

    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    const uint32_t available = kMaxMaterialsPerFrame - s_Data3D.MaterialBufferOffset;
    if (materials.size() > available)
    {
        // Draw the clamped tail with the default material instead of pointing
        // push constants at memory that was never written this frame.
        UHE_CORE_WARN("Material buffer exhausted: model needs {0} slots, {1} left this frame; "
                      "trailing primitives draw with the default material",
                      materials.size(), available);
    }

    const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(materials.size()), available);
    for (uint32_t i = 0; i < count; ++i)
    {
        RD3d::MaterialGPU gpu = RD3d::FillMaterialGPU(materials[i]);
        cmd.UpdateBuffer(s_Data3D.MaterialStorageBufferHandle, &gpu, sizeof(gpu),
                         (s_Data3D.MaterialBufferOffset + i) * sizeof(RD3d::MaterialGPU));
    }

    binding.BufferIndex = s_Data3D.MaterialStorageBufferIndex;
    binding.BaseIndex = s_Data3D.MaterialBufferOffset;
    binding.Count = count;
    s_Data3D.MaterialBufferOffset += count;
    return binding;
}

void Renderer3D::SubmitModel(const RD3d::Model& model, const glm::mat4& transform, int entityID,
                             const RD3d::Animator* animator)
{
    UHE_PROFILE_FUNCTION();
    // Upload bone matrices and material parameters once per model, then
    // forward the buffer locations to every sub-mesh.
    BoneBinding bones = PrepareBoneBinding(animator);
    MaterialBinding materials = PrepareMaterialBinding(model);

    for (const auto& mesh : model.GetMesh())
    {
        SubmitMesh(mesh, transform, entityID, model, materials, bones.BufferIndex, bones.Offset);
    }
}

// Issue #17: submit a single mesh (one glTF node) with its model's materials.
void Renderer3D::SubmitMesh(const RD3d::Mesh& mesh, const glm::mat4& transform, int entityID, const RD3d::Model& model,
                            const MaterialBinding& materialBinding, int boneBufferIndex, int boneOffset)
{
    UHE_PROFILE_FUNCTION();
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    // MUST stay byte-identical to PushConstants in Basic3D.slang. Material
    // values live in the per-material storage buffer; these 192 bytes only
    // carry where everything lives.
    struct PushConstants
    {
        glm::mat4 viewProj;
        glm::mat4 model;
        glm::vec4 cameraPos;
        int entityID;
        int enableLighting;
        int lightBufferIndex;
        int numLights;
        int boneBufferIndex;
        int boneOffset;
        int materialBufferIndex;
        int materialIndex;
        int useVertexColor;
        int viewMode; // Renderer3D::ViewMode
        int padding[2];
    } pc;

    static_assert(sizeof(PushConstants) == 192, "PushConstants must stay byte-identical to the Slang struct");
    static_assert(offsetof(PushConstants, materialBufferIndex) == 168,
                  "materialBufferIndex offset must match the Slang layout");

    // The pipeline's range came from the shader's reflection, so this is where a
    // host struct that drifted from the Slang one shows up -- before the upload
    // writes past the block the pipeline was built with.
    if (s_Data3D.PushConstantSize != sizeof(PushConstants))
    {
        UHE_CORE_ERROR("Renderer3D: push constants are {0} bytes in the shader and {1} in the host struct",
                       s_Data3D.PushConstantSize, sizeof(PushConstants));
    }

    pc.viewProj = s_Data3D.ViewProjection;
    pc.model = transform;
    pc.cameraPos = glm::vec4(s_Data3D.CameraPosition, 1.0f);
    pc.entityID = entityID;
    pc.enableLighting = s_Data3D.EnableLighting ? 1 : 0;
    pc.lightBufferIndex = s_Data3D.LightStorageBufferIndex;
    pc.numLights = static_cast<int>(std::min<size_t>(s_Data3D.CurrentLights.size(), kMaxLightsPerFrame));
    pc.boneBufferIndex = boneBufferIndex;
    pc.boneOffset = boneOffset;
    pc.materialBufferIndex = (materialBinding.BufferIndex >= 0) ? materialBinding.BufferIndex
                                                                : static_cast<int>(s_Data3D.MaterialStorageBufferIndex);
    pc.useVertexColor = 0;
    pc.viewMode = static_cast<int>(s_Data3D.Mode);
    pc.padding[0] = pc.padding[1] = 0;

    const auto& materials = model.GetMaterials();

    pc.model = transform * mesh.LocalTransform;

    for (const auto& prim : mesh.primitive)
    {
        if (!prim.VertexBuffer || !prim.IndexBuffer)
            continue;

        // glTF default material when the index is absent or out of range:
        // white base, fully rough, non-metallic. Slot 0 of the material buffer
        // holds exactly that, so out-of-range primitives just point at it.
        const RD3d::Material* material = nullptr;
        int materialIndex = 0; // default slot
        if (prim.materialIndex < materials.size())
        {
            material = &materials[prim.materialIndex];
            const int localIndex = static_cast<int>(prim.materialIndex);
            if (materialBinding.BaseIndex >= 0 && localIndex < static_cast<int>(materialBinding.Count))
                materialIndex = materialBinding.BaseIndex + localIndex;
            else
                material = nullptr; // buffer exhausted: draw with the default
        }

        pc.materialIndex = materialIndex;
        // COLOR_0 is a per-PRIMITIVE attribute in glTF, so the flag lives on
        // the primitive rather than the material: two materials in one mesh
        // can have vertex colours on different primitives.
        pc.useVertexColor = prim.hasVertexColor ? 1 : 0;

        // Cull mode lives in the pipeline, so a doubleSided material needs the
        // variant bound rather than a state change.
        const bool doubleSided = material && material->DoubleSided;
        const bool wireframe = s_Data3D.Mode == ViewMode::Wireframe;
        const auto* single = SingleTargetFor(s_Data3D.TargetFormat);
        if (single != nullptr)
            cmd.BindPipeline(wireframe ? single->ModelWireframe
                                       : (doubleSided ? single->ModelDoubleSided : single->Model));
        else
            cmd.BindPipeline(wireframe ? s_Data3D.ModelPipelineWireframe
                                       : (doubleSided ? s_Data3D.ModelPipelineDoubleSided : s_Data3D.ModelPipeline));

        cmd.PushConstants(RHI::ShaderStage::AllGraphics, &pc, sizeof(PushConstants), 0);

        cmd.BindVertexBuffer(prim.VertexBuffer);
        cmd.BindIndexBuffer(prim.IndexBuffer);
        cmd.DrawIndexed(prim.IndexCount);
    }
}

bool Renderer3D::IsLightingEnabled()
{
    return s_Data3D.EnableLighting;
}

void Renderer3D::SetLightingEnabled(bool enabled)
{
    s_Data3D.EnableLighting = enabled;
}

void Renderer3D::SetViewMode(ViewMode mode)
{
    s_Data3D.Mode = mode;
}

Renderer3D::ViewMode Renderer3D::GetViewMode()
{
    return s_Data3D.Mode;
}

bool Renderer3D::SupportsWireframe()
{
    return Renderer::GetDevice().SupportsWireframe();
}

} // namespace UHE
