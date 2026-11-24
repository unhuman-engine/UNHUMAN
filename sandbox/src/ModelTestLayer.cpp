#include "ModelTestLayer.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/Renderer/ShaderCache.h"
#include "UHE/Renderer3D/Renderer3D.h"

ModelTestLayer::ModelTestLayer() : Layer("ModelTestLayer") {}

void ModelTestLayer::OnAttach()
{
    // Shader edits show up while the sandbox runs, which is how the smoke test
    // covers hot reload without a restart.
    UHE::ShaderCache::Get().SetHotReloadEnabled(true);

    // Load the basic test GLTF
    std::string path = (UHE::FileSystem::Get().GetRootPath() / "../sandbox/assets/models/Box.gltf").string();
    if (!m_Model.loadModel(path))
    {
        UHE_CORE_ERROR("Failed to load test model!");
    }

    m_Camera = UHE::EditorCamera(30.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
    m_Camera.SetDistance(5.0f);
}

void ModelTestLayer::OnDetach()
{
    m_Model.Destroy();
}

void ModelTestLayer::OnUpdate(UHE::Timestep ts)
{
    // One-time smoke-test marker for CI. Logged on the *second* OnUpdate call:
    // by then frame 1 has been submitted AND presented (Device::End runs after
    // all layers), so a crash during the first present prevents the marker and
    // fails the CI wait. CI greps stdout for this line instead of a fixed
    // wall-clock timeout.
    static bool s_SeenFirstFrame = false;
    static bool s_ReadyLogged = false;
    if (!s_SeenFirstFrame)
    {
        s_SeenFirstFrame = true;
    }
    else if (!s_ReadyLogged)
    {
        s_ReadyLogged = true;
        UHE_CORE_INFO("[SMOKE-READY] first frame rendered");
    }

    m_Camera.OnUpdate(ts);

    // Update rotation
    m_Rotation += ts * 50.0f;

    // The scene is a frame graph pass: it declares what it renders into and
    // the graph owns the rest. The depth target is the graph's: it is sized
    // to the backbuffer every frame, so a resize needs no code here, and the
    // graph puts both attachments in the right state before the pass records.
    auto& device = UHE::Renderer::GetDevice();
    UHE::RHI::FrameGraph* graph = device.GetFrameGraph();
    if (graph == nullptr)
        return;

    using namespace UHE::RHI;
    const FGTexture::Desc backbuffer = device.GetBackbufferDesc();
    struct SceneData
    {
        FGHandle<FGTexture> Color;
        FGHandle<FGTexture> Depth;
    };
    const FGHandle<FGTexture> target = graph->ImportBackbuffer(backbuffer);
    const glm::mat4 transform = glm::scale(
        glm::rotate(glm::mat4(1.0f), glm::radians(m_Rotation), glm::vec3(0.0f, 1.0f, 0.0f)), glm::vec3(2.0f));

    graph->AddPass<SceneData>(
        "Sandbox.Scene",
        [&](FGBuilder& builder, SceneData& data)
        {
            data.Color = builder.Write(target, FGWrite::RenderTarget);
            const FGTexture::Desc depthDesc{backbuffer.Width, backbuffer.Height, TextureFormat::D24_UNORM_S8};
            data.Depth = builder.Write(builder.Create("Sandbox.Depth", depthDesc), FGWrite::DepthTarget);
        },
        [this, transform](const SceneData&, FGContext& context)
        {
            RHICommandBuffer& cmd = context.Cmd();
            cmd.SetViewport(0.0f, 0.0f, static_cast<float>(context.Width()), static_cast<float>(context.Height()));
            cmd.SetScissor(0, 0, context.Width(), context.Height());

            std::vector<UHE::RD3d::LightData> lights;
            UHE::RD3d::LightData defaultLight;
            defaultLight.Type_Radius_Pad = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // Type = 0 (Directional)
            defaultLight.PositionOrDirection = glm::vec4(0.5f, -1.0f, 0.3f, 0.0f);
            defaultLight.ColorIntensity = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
            lights.push_back(defaultLight);

            UHE::Renderer3D::SetSwapchainTarget(true);
            UHE::Renderer3D::BeginScene(m_Camera, lights);
            UHE::Renderer3D::SubmitModel(m_Model, transform);
            UHE::Renderer3D::EndScene();
        });
}

void ModelTestLayer::OnImGuiRender()
{
    ImGui::Begin("Model Test Info");
    ImGui::Text("Model Loaded: %s", m_Model.GetMesh().empty() ? "No" : "Yes");
    if (!m_Model.GetMesh().empty())
    {
        ImGui::Text("Mesh Name: %s", m_Model.GetMesh()[0].name.c_str());
        ImGui::Text("Vertices: %zu", m_Model.GetMesh()[0].primitive[0].vertices.size());
        ImGui::Text("Indices: %zu", m_Model.GetMesh()[0].primitive[0].indices.size());
    }
    ImGui::End();
}

void ModelTestLayer::OnEvent(UHE::Event& event)
{
    m_Camera.OnEvent(event);
}
