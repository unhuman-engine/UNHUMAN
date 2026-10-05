#include "ModelTestLayer.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include "UHE/RHI/RHICommandBuffer.h"

#include "UHE/Renderer3D/Renderer3D.h"

ModelTestLayer::ModelTestLayer() : Layer("ModelTestLayer") {}

void ModelTestLayer::OnAttach()
{
    // Load the basic test GLTF
    std::string path = (UHE::FileSystem::Get().GetRootPath() / "../sandbox/assets/models/Box.gltf").string();
    if (!m_Model.loadModel(path))
    {
        UHE_CORE_ERROR("Failed to load test model!");
    }

    m_Camera = UHE::EditorCamera(30.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
    m_Camera.SetDistance(5.0f);

    // Create a depth texture for the swapchain render pass
    auto& device = UHE::Renderer::GetDevice();
    uint32_t width = UHE::Application::Get().GetWindow().GetWidth();
    uint32_t height = UHE::Application::Get().GetWindow().GetHeight();

    UHE::RHI::TextureDesc depthDesc{};
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = UHE::RHI::TextureFormat::D24_UNORM_S8;
    depthDesc.usage = UHE::RHI::TextureUsage::DepthAttach | UHE::RHI::TextureUsage::Sampled;
    m_DepthTexture = device.CreateTexture(depthDesc);
}

void ModelTestLayer::OnDetach()
{
    auto& device = UHE::Renderer::GetDevice();
    m_Model.Destroy();
    if (m_DepthTexture)
    {
        device.DestroyTexture(m_DepthTexture);
        m_DepthTexture = nullptr;
    }
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

    auto& device = UHE::Renderer::GetDevice();
    auto& cmd = device.GetCurrentCommandBuffer();

    uint32_t width = UHE::Application::Get().GetWindow().GetWidth();
    uint32_t height = UHE::Application::Get().GetWindow().GetHeight();

    UHE::RHI::RenderPassDesc passDesc{};
    passDesc.renderWidth = width;
    passDesc.renderHeight = height;
    passDesc.colorAttachmentCount = 0; // Triggers Swapchain fallback
    passDesc.hasDepth = true;
    passDesc.depthAttachment.texture = m_DepthTexture;
    passDesc.depthAttachment.clearDepth = 1.0f;
    
    cmd.BeginRenderPass(passDesc);
    cmd.SetViewport(0.0f, 0.0f, (float)width, (float)height);
    cmd.SetScissor(0, 0, width, height);

    std::vector<UHE::RD3d::LightData> lights;
    UHE::RD3d::LightData defaultLight;
    defaultLight.Type_Radius_Pad = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // Type = 0 (Directional)
    defaultLight.PositionOrDirection = glm::vec4(0.5f, -1.0f, 0.3f, 0.0f);
    defaultLight.ColorIntensity = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    lights.push_back(defaultLight);

    UHE::Renderer3D::BeginScene(m_Camera, lights);

    // Render the model with some rotation
    glm::mat4 transform = glm::rotate(glm::mat4(1.0f), glm::radians(m_Rotation), glm::vec3(0.0f, 1.0f, 0.0f));
    transform = glm::scale(transform, glm::vec3(2.0f));

    UHE::Renderer3D::SubmitModel(m_Model, transform);

    UHE::Renderer3D::EndScene();

    cmd.EndRenderPass();
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
