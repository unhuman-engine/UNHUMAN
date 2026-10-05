#include "uhepch.h"
#include "Renderer3D.h"
#include <glm/gtc/matrix_transform.hpp>
#include "UHE/AssestsManager/VfsSystem.h"
#include "UHE/RHI/RHICommadBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/SlangCompiler.h"
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

// Fills the GPU struct from a loaded material. The mapping is deliberately
// dumb: the loader owns glTF semantics (including the engine-space UV
// transform conversion), the renderer only packs.
RD3d::MaterialGPU FillMaterialGPU(const RD3d::Material& material)
{
    RD3d::MaterialGPU gpu{};
    const auto& ext = material.Extensions;

    gpu.baseColorFactor = material.BaseColorFactor;
    gpu.emissiveFactorStrength = glm::vec4(material.EmissiveFactor, ext.EmissiveStrength);
    gpu.specularColorFactor = glm::vec4(ext.SpecularColorFactor, 1.0f);
    gpu.sheenColorFactor = glm::vec4(ext.SheenColorFactor, 1.0f);
    gpu.attenuationColor = glm::vec4(ext.AttenuationColor, 1.0f);
    gpu.diffuseTransmissionColor = glm::vec4(ext.DiffuseTransmissionColor, 1.0f);

    gpu.metallicFactor = material.MetallicFactor;
    gpu.roughnessFactor = material.RoughnessFactor;
    gpu.normalScale = material.NormalScale;
    gpu.occlusionStrength = material.OcclusionStrength;
    gpu.alphaCutoff = material.AlphaCutoff;
    gpu.ior = ext.IOR;
    gpu.specularFactor = ext.SpecularFactor;
    gpu.clearcoatFactor = ext.ClearcoatFactor;
    gpu.clearcoatRoughnessFactor = ext.ClearcoatRoughnessFactor;
    gpu.clearcoatNormalScale = ext.ClearcoatNormalScale;
    gpu.sheenRoughnessFactor = ext.SheenRoughnessFactor;
    gpu.transmissionFactor = ext.TransmissionFactor;
    gpu.thicknessFactor = ext.ThicknessFactor;
    gpu.attenuationDistance = ext.AttenuationDistance;
    gpu.diffuseTransmissionFactor = ext.DiffuseTransmissionFactor;
    gpu.iridescenceFactor = ext.IridescenceFactor;
    gpu.iridescenceIor = ext.IridescenceIOR;
    gpu.iridescenceThicknessMinimum = ext.IridescenceThicknessMinimum;
    gpu.iridescenceThicknessMaximum = ext.IridescenceThicknessMaximum;
    gpu.anisotropyStrength = ext.AnisotropyStrength;
    gpu.anisotropyRotation = ext.AnisotropyRotation;

    uint32_t features = 0;
    if (ext.HasClearcoat) features |= RD3d::kFeatureClearcoat;
    if (ext.HasSpecular) features |= RD3d::kFeatureSpecular;
    if (ext.HasSheen) features |= RD3d::kFeatureSheen;
    if (ext.HasTransmission) features |= RD3d::kFeatureTransmission;
    if (ext.HasTransmission && ext.ThicknessFactor > 0.0f) features |= RD3d::kFeatureVolume;
    if (ext.HasIridescence) features |= RD3d::kFeatureIridescence;
    if (ext.HasAnisotropy) features |= RD3d::kFeatureAnisotropy;
    if (ext.HasDiffuseTransmission) features |= RD3d::kFeatureDiffuseTransmission;

    gpu.flags = glm::ivec4(static_cast<int>(material.Alpha), ext.Unlit ? 1 : 0,
                           static_cast<int>(features), 0);

    // Texture slots. Each carries its own (already engine-space) UV transform,
    // so KHR_texture_transform applies per map, not per material.
    const Ref<Texture2D> slotTextures[] = {
        material.AlbedoTexture,             material.MetallicRoughnessTexture,
        material.NormalTexture,             material.OcclusionTexture,
        material.EmissiveTexture,           ext.ClearcoatTexture,
        ext.ClearcoatRoughnessTexture,      ext.ClearcoatNormalTexture,
        ext.SpecularTexture,                ext.SpecularColorTexture,
        ext.SheenColorTexture,              ext.SheenRoughnessTexture,
        ext.TransmissionTexture,            ext.ThicknessTexture,
        ext.IridescenceTexture,             ext.IridescenceThicknessTexture,
        ext.AnisotropyTexture,              ext.DiffuseTransmissionTexture,
        ext.DiffuseTransmissionColorTexture,
    };
    static_assert(std::size(slotTextures) == RD3d::kSlotCount, "slot table must cover every GPU slot");

    for (int slot = 0; slot < RD3d::kSlotCount; ++slot)
    {
        auto& out = gpu.slots[slot];
        const auto& transform = material.UVTransforms[slot];
        if (slotTextures[slot])
        {
            out.index = glm::ivec4(static_cast<int>(slotTextures[slot]->GetTextureIndex()), 0, 0, 0);
        }
        else
        {
            out.index = glm::ivec4(-1, 0, 0, 0);
        }
        out.rotationScale =
            glm::vec4(transform.cosRotation, transform.sinRotation, transform.scale.x, transform.scale.y);
        out.offset = glm::vec4(transform.offset, 0.0f, 0.0f);
    }

    return gpu;
}

} // namespace

struct Renderer3DData
{
    RHI::ShaderHandle VertexShader;
    RHI::ShaderHandle FragmentShader;
    RHI::PipelineHandle ModelPipeline;
    // Same shaders and layout, cull mode None. Cull mode is baked into the
    // pipeline, so a doubleSided material cannot be drawn with ModelPipeline -
    // it needs this variant bound instead.
    RHI::PipelineHandle ModelPipelineDoubleSided;

    RHI::ShaderHandle GridVertexShader;
    RHI::ShaderHandle GridFragmentShader;
    RHI::PipelineHandle GridPipeline;

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

    std::string shaderPath = (FileSystem::Get().GetRootPath() / "assets/shaders/Basic3D.slang").string();
    auto compiledShaders = SlangCompiler::CompileToSPIRV(shaderPath);

    if (compiledShaders.find(RHI::ShaderStage::Vertex) != compiledShaders.end())
    {
        RHI::ShaderDesc vsDesc{};
        vsDesc.stage = RHI::ShaderStage::Vertex;
        vsDesc.spirvData = (const uint8_t*)compiledShaders[RHI::ShaderStage::Vertex].data();
        vsDesc.spirvSize = compiledShaders[RHI::ShaderStage::Vertex].size();
        s_Data3D.VertexShader = device.CreateShader(vsDesc);
    }

    if (compiledShaders.find(RHI::ShaderStage::Fragment) != compiledShaders.end())
    {
        RHI::ShaderDesc fsDesc{};
        fsDesc.stage = RHI::ShaderStage::Fragment;
        fsDesc.spirvData = (const uint8_t*)compiledShaders[RHI::ShaderStage::Fragment].data();
        fsDesc.spirvSize = compiledShaders[RHI::ShaderStage::Fragment].size();
        s_Data3D.FragmentShader = device.CreateShader(fsDesc);
    }

    RHI::GraphicsPipelineDesc pipeDesc{};
    pipeDesc.vertexShader = s_Data3D.VertexShader;
    pipeDesc.fragmentShader = s_Data3D.FragmentShader;
    // Location 5 (a_Tangent) must match VertexInput in Basic3D.slang.
    pipeDesc.vertexLayout = {{RHI::ShaderDataType::Float3, "a_Position"},
                             {RHI::ShaderDataType::Float3, "a_Normal"},
                             {RHI::ShaderDataType::Float2, "a_TexCoord"},
                             {RHI::ShaderDataType::Int4, "a_Joints"},
                             {RHI::ShaderDataType::Float4, "a_Weights"},
                             {RHI::ShaderDataType::Float4, "a_Tangent"},
                             {RHI::ShaderDataType::Float4, "a_Color"}};

    // Push constants only carry where things live (camera, bones, lights, and
    // the material's buffer location); the material's VALUES moved to the
    // per-material storage buffer because this block was at the 256-byte
    // device limit with no room for one more field.
    pipeDesc.pushConstantSize = 192;
    pipeDesc.blendMode = RHI::BlendMode::Alpha;
    pipeDesc.depthTest = true;
    pipeDesc.depthWrite = true;

    pipeDesc.colorAttachmentCount = 2;
    pipeDesc.colorFormats[0] = RHI::TextureFormat::RGBA8_SRGB;
    pipeDesc.colorFormats[1] = RHI::TextureFormat::R32_SINT;

    s_Data3D.ModelPipeline = device.CreateGraphicsPipeline(pipeDesc);

    // Double-sided variant: identical except culling, selected per material.
    RHI::GraphicsPipelineDesc doubleSidedDesc = pipeDesc;
    doubleSidedDesc.cullMode = RHI::CullMode::None;
    doubleSidedDesc.debugName = "ModelPipeline.DoubleSided";
    s_Data3D.ModelPipelineDoubleSided = device.CreateGraphicsPipeline(doubleSidedDesc);

    // Initialize Grid Pipeline
    std::string gridShaderPath = (FileSystem::Get().GetRootPath() / "assets/shaders/Grid.slang").string();
    auto compiledGridShaders = SlangCompiler::CompileToSPIRV(gridShaderPath);

    if (compiledGridShaders.find(RHI::ShaderStage::Vertex) != compiledGridShaders.end())
    {
        RHI::ShaderDesc vsDesc{};
        vsDesc.stage = RHI::ShaderStage::Vertex;
        vsDesc.spirvData = (const uint8_t*)compiledGridShaders[RHI::ShaderStage::Vertex].data();
        vsDesc.spirvSize = compiledGridShaders[RHI::ShaderStage::Vertex].size();
        s_Data3D.GridVertexShader = device.CreateShader(vsDesc);
    }

    if (compiledGridShaders.find(RHI::ShaderStage::Fragment) != compiledGridShaders.end())
    {
        RHI::ShaderDesc fsDesc{};
        fsDesc.stage = RHI::ShaderStage::Fragment;
        fsDesc.spirvData = (const uint8_t*)compiledGridShaders[RHI::ShaderStage::Fragment].data();
        fsDesc.spirvSize = compiledGridShaders[RHI::ShaderStage::Fragment].size();
        s_Data3D.GridFragmentShader = device.CreateShader(fsDesc);
    }

    RHI::GraphicsPipelineDesc gridPipeDesc{};
    gridPipeDesc.vertexShader = s_Data3D.GridVertexShader;
    gridPipeDesc.fragmentShader = s_Data3D.GridFragmentShader;
    gridPipeDesc.vertexLayout = {}; // Empty vertex layout, using gl_VertexIndex

    gridPipeDesc.pushConstantSize = sizeof(glm::mat4) * 2; // viewProj + inverseViewProj
    gridPipeDesc.blendMode = RHI::BlendMode::Alpha;
    gridPipeDesc.depthTest = true;
    gridPipeDesc.depthWrite = true;

    gridPipeDesc.colorAttachmentCount = 2;
    gridPipeDesc.colorFormats[0] = RHI::TextureFormat::RGBA8_SRGB;
    gridPipeDesc.colorFormats[1] = RHI::TextureFormat::R32_SINT;

    s_Data3D.GridPipeline = device.CreateGraphicsPipeline(gridPipeDesc);

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
    s_Data3D.MaterialStorageBufferIndex = device.GetBufferBindlessIndex(s_Data3D.MaterialStorageBufferHandle);
    s_Data3D.MaterialBufferOffset = 1; // Slot 0 is the default material, written each BeginScene.
}

void Renderer3D::Shutdown()
{
    auto& device = Renderer::GetDevice();
    device.DestroyGraphicsPipeline(s_Data3D.ModelPipeline);
    device.DestroyGraphicsPipeline(s_Data3D.ModelPipelineDoubleSided);
    device.DestroyShader(s_Data3D.VertexShader);
    device.DestroyShader(s_Data3D.FragmentShader);

    device.DestroyGraphicsPipeline(s_Data3D.GridPipeline);
    device.DestroyShader(s_Data3D.GridVertexShader);
    device.DestroyShader(s_Data3D.GridFragmentShader);

    device.DestroyBuffer(s_Data3D.LightStorageBufferHandle);
    device.DestroyBuffer(s_Data3D.BoneStorageBufferHandle);
    device.DestroyBuffer(s_Data3D.MaterialStorageBufferHandle);

    s_Data3D.WhiteTexture.reset();
}

void Renderer3D::BeginScene(const EditorCamera& camera, const std::vector<RD3d::LightData>& lights)
{
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
    RD3d::MaterialGPU defaultGPU = FillMaterialGPU(defaultMaterial);
    cmd.UpdateBuffer(s_Data3D.MaterialStorageBufferHandle, &defaultGPU, sizeof(defaultGPU), 0);

    s_Data3D.MaterialBufferOffset = 1;
}

void Renderer3D::EndScene() {}

void Renderer3D::DrawGrid()
{
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    cmd.BindPipeline(s_Data3D.GridPipeline);

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
    const auto& materials = model.GetMaterials();
    if (materials.empty())
        return binding; // draws fall back to the default material

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
        RD3d::MaterialGPU gpu = FillMaterialGPU(materials[i]);
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
void Renderer3D::SubmitMesh(const RD3d::Mesh& mesh, const glm::mat4& transform, int entityID,
                            const RD3d::Model& model, const MaterialBinding& materialBinding,
                            int boneBufferIndex, int boneOffset)
{
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
    pc.viewProj = s_Data3D.ViewProjection;
    pc.model = transform;
    pc.cameraPos = glm::vec4(s_Data3D.CameraPosition, 1.0f);
    pc.entityID = entityID;
    pc.enableLighting = s_Data3D.EnableLighting ? 1 : 0;
    pc.lightBufferIndex = s_Data3D.LightStorageBufferIndex;
    pc.numLights = static_cast<int>(std::min<size_t>(s_Data3D.CurrentLights.size(), 1024));
    pc.boneBufferIndex = boneBufferIndex;
    pc.boneOffset = boneOffset;
    pc.materialBufferIndex = materialBinding.BufferIndex;
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
        cmd.BindPipeline(doubleSided ? s_Data3D.ModelPipelineDoubleSided : s_Data3D.ModelPipeline);

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
