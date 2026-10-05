#include "uhepch.h"
#include "Renderer3D.h"
#include <glm/gtc/matrix_transform.hpp>
#include "UHE/AssestsManager/VfsSystem.h"
#include "UHE/RHI/RHICommandBuffer.h"
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
    pipeDesc.vertexLayout = {{RHI::ShaderDataType::Float3, "a_Position"},
                             {RHI::ShaderDataType::Float3, "a_Normal"},
                             {RHI::ShaderDataType::Float2, "a_TexCoord"},
                             {RHI::ShaderDataType::Int4, "a_Joints"},
                             {RHI::ShaderDataType::Float4, "a_Weights"}};

    pipeDesc.pushConstantSize = 192;
    pipeDesc.blendMode = RHI::BlendMode::Alpha;
    pipeDesc.depthTest = true;
    pipeDesc.depthWrite = true;

    pipeDesc.colorAttachmentCount = 2;
    pipeDesc.colorFormats[0] = RHI::TextureFormat::RGBA8_SRGB;
    pipeDesc.colorFormats[1] = RHI::TextureFormat::R32_SINT;

    s_Data3D.ModelPipeline = device.CreateGraphicsPipeline(pipeDesc);

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
                            const std::vector<RD3d::Material>& materials, int boneBufferIndex, int boneOffset)
{
    auto& cmd = Renderer::GetDevice().GetCurrentCommandBuffer();

    cmd.BindPipeline(s_Data3D.ModelPipeline);

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
        int padding1;
        int padding2;
    } pc;
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

    for (const auto& prim : mesh.primitive)
    {
        if (!prim.VertexBuffer || !prim.IndexBuffer)
            continue;

        int textureSlot = s_Data3D.WhiteTexture->GetTextureIndex(); // Default to white texture
        int mrTextureSlot = -1;
        float metallicFactor = 0.0f;
        float roughnessFactor = 0.4f;

        if (prim.materialIndex < materials.size())
        {
            auto& material = materials[prim.materialIndex];
            if (material.AlbedoTexture)
            {
                textureSlot = material.AlbedoTexture->GetTextureIndex();
            }
            if (material.MetallicRoughnessTexture)
            {
                mrTextureSlot = material.MetallicRoughnessTexture->GetTextureIndex();
            }
            metallicFactor = material.MetallicFactor;
            roughnessFactor = material.RoughnessFactor;
        }

        pc.textureSlot = textureSlot;
        pc.mrTextureSlot = mrTextureSlot;
        pc.metallicFactor = metallicFactor;
        pc.roughnessFactor = roughnessFactor;
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
