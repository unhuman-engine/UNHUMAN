#include "uhepch.h"
#include "Renderer3D.h"
#include <glm/gtc/matrix_transform.hpp>
#include "UHE/AssestsManager/VfsSystem.h"
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

    RHI::ShaderHandle GridVertexShader;
    RHI::ShaderHandle GridFragmentShader;
    RHI::PipelineHandle GridPipeline;

    // The editor draws the scene into its own framebuffer, which is a colour
    // target plus the entity-id target; the game path draws it straight into
    // the swapchain, which is one attachment in the swapchain's own format. A
    // pipeline has to match the pass it is bound in, so those passes get their
    // own variants rather than the two-attachment RGBA8 one above.
    RHI::PipelineHandle ModelPipelineSwapchain;
    RHI::PipelineHandle ModelPipelineSwapchainDoubleSided;
    RHI::PipelineHandle GridPipelineSwapchain;
    bool SwapchainTarget = false;

    /// Push constant bytes the pipelines were built with, read from the shader's
    /// reflection. Compared against the host struct before every upload, so a
    /// shader and a struct that drift apart are reported instead of writing past
    /// the block.
    u32 PushConstantSize = 0;

    glm::mat4 ViewProjection;
    glm::vec3 CameraPosition;
    std::vector<RD3d::LightData> CurrentLights;
    Ref<Texture2D> WhiteTexture;

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
};

static Renderer3DData s_Data3D;

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

    RHI::BufferDesc desc;
    desc.size = sizeof(RD3d::LightData) * 1024; // Limit to 1024 lights
    desc.usage = RHI::BufferUsage::Storage;
    desc.hostVisible = true;
    s_Data3D.LightStorageBufferHandle = device.CreateBuffer(desc);
    s_Data3D.LightStorageBufferIndex = device.GetBufferBindlessIndex(s_Data3D.LightStorageBufferHandle);

    RHI::BufferDesc boneBufferDesc{};
    boneBufferDesc.size = sizeof(glm::mat4) * 4096; // Support up to 4096 bones per frame
    boneBufferDesc.usage = RHI::BufferUsage::Storage;
    boneBufferDesc.hostVisible = true;
    s_Data3D.BoneStorageBufferHandle = device.CreateBuffer(boneBufferDesc);
    s_Data3D.BoneStorageBufferIndex = device.GetBufferBindlessIndex(s_Data3D.BoneStorageBufferHandle);

    RHI::BufferDesc materialBufferDesc{};
    materialBufferDesc.size = sizeof(RD3d::MaterialGPU) * kMaxMaterialsPerFrame;
    materialBufferDesc.usage = RHI::BufferUsage::Storage;
    materialBufferDesc.hostVisible = true;
    s_Data3D.MaterialStorageBufferHandle = device.CreateBuffer(materialBufferDesc);
    // The material array lives in its own bindless namespace (shader binding
    // 2), separate from the light/bone arrays of binding 0.
    s_Data3D.MaterialStorageBufferIndex = device.GetMaterialBufferBindlessIndex(s_Data3D.MaterialStorageBufferHandle);
    s_Data3D.MaterialBufferOffset = 1; // Slot 0 is the default material, written each BeginScene.
}

void Renderer3D::Shutdown()
{
    auto& device = Renderer::GetDevice();
    DestroyShaderResources();
    device.DestroyBuffer(s_Data3D.LightStorageBufferHandle);
    device.DestroyBuffer(s_Data3D.BoneStorageBufferHandle);
    device.DestroyBuffer(s_Data3D.MaterialStorageBufferHandle);

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
    const ShaderCache::Handle modelNoIdHandle =
        library.Load((shaderDir / "Basic3D.slang").string(), "no_entity_id");
    const ShaderCache::Handle gridHandle = library.Load((shaderDir / "Grid.slang").string());

    const ShaderCache::Shader* model = library.Get(modelHandle);
    const ShaderCache::Shader* modelNoId = library.Get(modelNoIdHandle);
    const ShaderCache::Shader* grid = library.Get(gridHandle);
    if (model == nullptr || modelNoId == nullptr || grid == nullptr)
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

    RHI::GraphicsPipelineDesc gridPipeDesc{};
    gridPipeDesc.vertexShader = s_Data3D.GridVertexShader;
    gridPipeDesc.fragmentShader = s_Data3D.GridFragmentShader;
    // The grid generates its vertices from gl_VertexIndex, so the module declares
    // no vertex inputs and reflection returns an empty layout.
    gridPipeDesc.vertexLayout = ReflectedVertexLayout(*grid);
    gridPipeDesc.pushConstantSize = ReflectedPushConstantSize(*grid); // viewProj + inverseViewProj
    gridPipeDesc.blendMode = RHI::BlendMode::Alpha;
    gridPipeDesc.depthTest = true;
    gridPipeDesc.depthWrite = true;

    gridPipeDesc.colorAttachmentCount = 2;
    gridPipeDesc.colorFormats[0] = RHI::TextureFormat::RGBA8_SRGB;
    gridPipeDesc.colorFormats[1] = RHI::TextureFormat::R32_SINT;

    s_Data3D.GridPipeline = device.CreateGraphicsPipeline(gridPipeDesc);

    // Swapchain variants: one colour attachment, in the swapchain's format. The
    // fragment shader still writes Location 1 (the entity id), which is simply
    // unused in a pass that has no second attachment -- legal, unlike binding a
    // two-attachment pipeline into a one-attachment pass.
    RHI::GraphicsPipelineDesc swapchainDesc = pipeDesc;
    swapchainDesc.colorAttachmentCount = 1;
    swapchainDesc.colorFormats[0] = device.GetSwapchainFormat();
    swapchainDesc.fragmentShader = s_Data3D.FragmentShaderNoId;
    swapchainDesc.debugName = "ModelPipeline.Swapchain";
    s_Data3D.ModelPipelineSwapchain = device.CreateGraphicsPipeline(swapchainDesc);

    RHI::GraphicsPipelineDesc swapchainDoubleSidedDesc = swapchainDesc;
    swapchainDoubleSidedDesc.cullMode = RHI::CullMode::None;
    swapchainDoubleSidedDesc.debugName = "ModelPipeline.Swapchain.DoubleSided";
    s_Data3D.ModelPipelineSwapchainDoubleSided = device.CreateGraphicsPipeline(swapchainDoubleSidedDesc);

    RHI::GraphicsPipelineDesc gridSwapchainDesc = gridPipeDesc;
    gridSwapchainDesc.colorAttachmentCount = 1;
    gridSwapchainDesc.colorFormats[0] = device.GetSwapchainFormat();
    gridSwapchainDesc.debugName = "GridPipeline.Swapchain";
    s_Data3D.GridPipelineSwapchain = device.CreateGraphicsPipeline(gridSwapchainDesc);
}

void Renderer3D::DestroyShaderResources()
{
    auto& device = Renderer::GetDevice();

    device.DestroyGraphicsPipeline(s_Data3D.ModelPipeline);
    device.DestroyGraphicsPipeline(s_Data3D.ModelPipelineDoubleSided);
    device.DestroyGraphicsPipeline(s_Data3D.ModelPipelineSwapchain);
    device.DestroyGraphicsPipeline(s_Data3D.ModelPipelineSwapchainDoubleSided);
    device.DestroyGraphicsPipeline(s_Data3D.GridPipeline);
    device.DestroyGraphicsPipeline(s_Data3D.GridPipelineSwapchain);
    device.DestroyShader(s_Data3D.VertexShader);
    device.DestroyShader(s_Data3D.FragmentShader);
    device.DestroyShader(s_Data3D.FragmentShaderNoId);
    device.DestroyShader(s_Data3D.GridVertexShader);
    device.DestroyShader(s_Data3D.GridFragmentShader);

    // Nulled, not left dangling: a second destroy of the same handle would free
    // someone else's object.
    s_Data3D.ModelPipeline = nullptr;
    s_Data3D.ModelPipelineDoubleSided = nullptr;
    s_Data3D.ModelPipelineSwapchain = nullptr;
    s_Data3D.ModelPipelineSwapchainDoubleSided = nullptr;
    s_Data3D.GridPipeline = nullptr;
    s_Data3D.GridPipelineSwapchain = nullptr;
    s_Data3D.VertexShader = nullptr;
    s_Data3D.FragmentShader = nullptr;
    s_Data3D.FragmentShaderNoId = nullptr;
    s_Data3D.GridVertexShader = nullptr;
    s_Data3D.GridFragmentShader = nullptr;
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
    s_Data3D.CurrentLights = lights;
    if (!lights.empty())
    {
        Renderer::GetDevice().GetCurrentCommandBuffer().UpdateBuffer(s_Data3D.LightStorageBufferHandle, lights.data(),
                                                                     lights.size() * sizeof(RD3d::LightData));
    }
    s_Data3D.BoneBufferOffset = 0;

    RestartMaterialRegion();
}

void Renderer3D::BeginScene(const Camera& camera, const glm::mat4& transform,
                            const std::vector<RD3d::LightData>& lights)
{
    UHE_PROFILE_FUNCTION();
    s_Data3D.ViewProjection = camera.GetProjection() * glm::inverse(transform);
    s_Data3D.CameraPosition = glm::vec3(transform[3]);
    s_Data3D.CurrentLights = lights;
    if (!lights.empty())
    {
        Renderer::GetDevice().GetCurrentCommandBuffer().UpdateBuffer(s_Data3D.LightStorageBufferHandle, lights.data(),
                                                                     lights.size() * sizeof(RD3d::LightData));
    }
    s_Data3D.BoneBufferOffset = 0;

    RestartMaterialRegion();
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

void Renderer3D::SetSwapchainTarget(bool enabled) { s_Data3D.SwapchainTarget = enabled; }

void Renderer3D::DrawGrid()
{
    UHE_PROFILE_FUNCTION();
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    cmd.BindPipeline(s_Data3D.SwapchainTarget ? s_Data3D.GridPipelineSwapchain : s_Data3D.GridPipeline);

    struct GridPushConstants
    {
        glm::mat4 viewProj;
        glm::mat4 inverseViewProj;
    } pc;
    pc.viewProj = s_Data3D.ViewProjection;
    pc.inverseViewProj = glm::inverse(s_Data3D.ViewProjection);

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
        if (!matrices.empty())
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
        glm::ivec3 padding;
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
    pc.numLights = static_cast<int>(std::min<size_t>(s_Data3D.CurrentLights.size(), 1024));
    pc.boneBufferIndex = boneBufferIndex;
    pc.boneOffset = boneOffset;
    pc.materialBufferIndex = (materialBinding.BufferIndex >= 0) ? materialBinding.BufferIndex
                                                                : static_cast<int>(s_Data3D.MaterialStorageBufferIndex);
    pc.useVertexColor = 0;
    pc.padding = glm::ivec3(0);

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
        cmd.BindPipeline(
            s_Data3D.SwapchainTarget
                ? (doubleSided ? s_Data3D.ModelPipelineSwapchainDoubleSided : s_Data3D.ModelPipelineSwapchain)
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

} // namespace UHE
