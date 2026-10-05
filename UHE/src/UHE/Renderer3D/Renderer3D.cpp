#include "uhepch.h"
#include "Renderer3D.h"
#include <glm/gtc/matrix_transform.hpp>
#include "UHE/AssestsManager/VfsSystem.h"
#include "UHE/RHI/RHICommadBuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/Renderer/Renderer.h"
#include "UHE/Renderer/SlangCompiler.h"

namespace UHE
{

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

    // 256 bytes, which is EXACTLY maxPushConstantsSize on this device (vulkaninfo,
    // GFX9). There is no headroom left: a fourth texture map or any new material
    // field now requires moving material parameters into a per-material descriptor
    // rather than another push-constant slot.
    pipeDesc.pushConstantSize = 256;
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

void Renderer3D::SubmitModel(const RD3d::Model& model, const glm::mat4& transform, int entityID,
                             const RD3d::Animator* animator)
{
    // Upload bone matrices once per model, then forward the buffer location to
    // every sub-mesh.
    BoneBinding bones = PrepareBoneBinding(animator);

    for (const auto& mesh : model.GetMesh())
    {
        SubmitMesh(mesh, transform, entityID, model.GetMaterials(), bones.BufferIndex, bones.Offset);
    }
}

// Issue #17: submit a single mesh (one glTF node) with its model's materials.
void Renderer3D::SubmitMesh(const RD3d::Mesh& mesh, const glm::mat4& transform, int entityID,
                            const std::vector<RD3d::Material>& materials, int boneBufferIndex,
                            int boneOffset)
{
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    // MUST stay byte-identical to PushConstants in Basic3D.slang. Slang and C++
    // have different default alignments for a trailing float3, so the explicit
    // padding is what keeps emissiveFactor at the same offset in both.
    struct PushConstants
    {
        glm::mat4 viewProj;
        glm::mat4 model;
        glm::vec4 cameraPos;
        int entityID;
        int textureSlot;
        int enableLighting;
        int lightBufferIndex;
        int numLights;
        int mrTextureSlot;
        float metallicFactor;
        float roughnessFactor;
        int boneBufferIndex;
        int boneOffset;

        int normalTextureSlot;
        int occlusionTextureSlot;
        int emissiveTextureSlot;
        float normalScale;
        float occlusionStrength;
        int alphaCutoff;
        int alphaMode;
        // 12 bytes of explicit padding, NOT an assumption that the compiler will
        // insert it. Slang lays this block out in std430: alphaMode ends at 212,
        // and baseColorFactor must sit on a 16-byte boundary, so it starts at
        // 224. C++ would pack it straight after alphaMode at 212 and disagree by
        // 12 bytes - which reads as plausible garbage in every material field
        // rather than as a validation error. The matching static_assert below
        // pins both offsets against the compiled SPIR-V layout.
        //
        // One of those 12 spare bytes is spent here on useVertexColor. The struct
        // is already at maxPushConstantsSize (256 on GFX9), so a new flag has to
        // come out of existing padding - there is no room to append one.
        int useVertexColor;
        float padding[2];
        glm::vec4 baseColorFactor;
        glm::vec4 emissiveFactor;
    } pc;

    static_assert(sizeof(PushConstants) == 256, "PushConstants must stay byte-identical to the Slang struct");
    static_assert(offsetof(PushConstants, baseColorFactor) == 224, "baseColorFactor offset must match std430");
    static_assert(offsetof(PushConstants, emissiveFactor) == 240, "emissiveFactor offset must match std430");
    pc.viewProj = s_Data3D.ViewProjection;
    pc.model = transform;
    pc.cameraPos = glm::vec4(s_Data3D.CameraPosition, 1.0f);
    pc.entityID = entityID;
    pc.textureSlot = 0; // Temp hardcode until material system is done
    pc.enableLighting = s_Data3D.EnableLighting ? 1 : 0;
    pc.lightBufferIndex = s_Data3D.LightStorageBufferIndex;
    pc.numLights = static_cast<int>(s_Data3D.CurrentLights.size());
    pc.mrTextureSlot = -1;
    pc.metallicFactor = 1.0f;
    pc.roughnessFactor = 1.0f;
    pc.boneBufferIndex = boneBufferIndex;
    pc.boneOffset = boneOffset;
    pc.normalTextureSlot = -1;
    pc.occlusionTextureSlot = -1;
    pc.emissiveTextureSlot = -1;
    pc.normalScale = 1.0f;
    pc.occlusionStrength = 1.0f;
    pc.alphaCutoff = 0.5f;
    pc.alphaMode = 0;
    pc.useVertexColor = 0;
    pc.baseColorFactor = glm::vec4(1.0f);
    pc.emissiveFactor = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);


    pc.model = transform * mesh.LocalTransform;

    for (const auto& prim : mesh.primitive)
    {
        if (!prim.VertexBuffer || !prim.IndexBuffer)
            continue;

        // glTF default material when the index is absent or out of range:
        // white base, fully rough, non-metallic - the same values the loader
        // uses as struct defaults.
        int textureSlot = s_Data3D.WhiteTexture->GetTextureIndex();
        int mrTextureSlot = -1;
        int normalSlot = -1;
        int occlusionSlot = -1;
        int emissiveSlot = -1;
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
        float alphaCutoff = 0.5f;
        int alphaMode = 0;
        glm::vec4 baseColorFactor(1.0f);
        glm::vec4 emissiveFactor(0.0f, 0.0f, 0.0f, 0.0f);
        bool doubleSided = false;
        // COLOR_0 is a per-PRIMITIVE attribute in glTF, so the flag lives on
        // the primitive rather than the material: two materials in one mesh
        // can have vertex colours on different primitives.
        int useVertexColor = prim.hasVertexColor ? 1 : 0;

        if (prim.materialIndex < materials.size())
        {
            const auto& material = materials[prim.materialIndex];
            if (material.AlbedoTexture)
                textureSlot = material.AlbedoTexture->GetTextureIndex();
            if (material.MetallicRoughnessTexture)
                mrTextureSlot = material.MetallicRoughnessTexture->GetTextureIndex();
            if (material.NormalTexture)
                normalSlot = material.NormalTexture->GetTextureIndex();
            if (material.OcclusionTexture)
                occlusionSlot = material.OcclusionTexture->GetTextureIndex();
            if (material.EmissiveTexture)
                emissiveSlot = material.EmissiveTexture->GetTextureIndex();

            metallicFactor = material.MetallicFactor;
            roughnessFactor = material.RoughnessFactor;
            normalScale = material.NormalScale;
            occlusionStrength = material.OcclusionStrength;
            alphaCutoff = material.AlphaCutoff;
            alphaMode = static_cast<int>(material.Alpha);
            baseColorFactor = material.BaseColorFactor;
            emissiveFactor = glm::vec4(material.EmissiveFactor, 0.0f);
            doubleSided = material.DoubleSided;
        }

        // Cull mode lives in the pipeline, so a doubleSided material needs the
        // variant bound rather than a state change.
        cmd.BindPipeline(doubleSided ? s_Data3D.ModelPipelineDoubleSided : s_Data3D.ModelPipeline);

        pc.textureSlot = textureSlot;
        pc.mrTextureSlot = mrTextureSlot;
        pc.metallicFactor = metallicFactor;
        pc.roughnessFactor = roughnessFactor;
        pc.normalTextureSlot = normalSlot;
        pc.occlusionTextureSlot = occlusionSlot;
        pc.emissiveTextureSlot = emissiveSlot;
        pc.normalScale = normalScale;
        pc.occlusionStrength = occlusionStrength;
        pc.alphaCutoff = alphaCutoff;
        pc.alphaMode = alphaMode;
        pc.useVertexColor = useVertexColor;
        pc.baseColorFactor = baseColorFactor;
        pc.emissiveFactor = emissiveFactor;
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
