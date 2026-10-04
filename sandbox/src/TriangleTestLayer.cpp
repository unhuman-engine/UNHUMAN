#include "TriangleTestLayer.h"
#include <vector>
#include "TriangleShaders.h"

#include "UHE/RHI/RHICommandBuffer.h"
#include "Platform/Vulkan/VulkanDevice.h"

using namespace UHE;

struct Vertex {
    float position[3];
    float color[3];
};

TriangleTestLayer::TriangleTestLayer() : Layer("TriangleTestLayer") {}

void TriangleTestLayer::OnAttach()
{
    auto& device = Renderer::GetDevice();

    // 1. Create Vertex Buffer
    std::vector<Vertex> vertices = {
        {{ 0.0f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        {{ 0.5f,  0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}}
    };

    RHI::BufferDesc vboDesc{};
    vboDesc.size = vertices.size() * sizeof(Vertex);
    vboDesc.usage = RHI::BufferUsage::Vertex;
    vboDesc.hostVisible = true;
    m_VertexBuffer = device.CreateBuffer(vboDesc);

    // 2. Create Shaders
    RHI::ShaderDesc vertDesc{};
    vertDesc.stage = RHI::ShaderStage::Vertex;
    vertDesc.spirvData = vert_spv;
    vertDesc.spirvSize = vert_spv_len;
    vertDesc.entryPoint = "main";
    m_VertexShader = device.CreateShader(vertDesc);

    RHI::ShaderDesc fragDesc{};
    fragDesc.stage = RHI::ShaderStage::Fragment;
    fragDesc.spirvData = frag_spv;
    fragDesc.spirvSize = frag_spv_len;
    fragDesc.entryPoint = "main";
    m_FragmentShader = device.CreateShader(fragDesc);

    // 3. Create Graphics Pipeline
    RHI::GraphicsPipelineDesc pipeDesc{};
    pipeDesc.vertexShader = m_VertexShader;
    pipeDesc.fragmentShader = m_FragmentShader;
    pipeDesc.vertexLayout = {
        { RHI::ShaderDataType::Float3, "inPosition" },
        { RHI::ShaderDataType::Float3, "inColor" }
    };
    pipeDesc.topology = RHI::PrimitiveTopology::TriangleList;
    // pipeDesc.colorFormat was here
    // Swapchain format is usually BGRA8_SRGB or BGRA8_UNORM. Let's assume BGRA8_UNORM or SRGB works.
    auto& swapChain = static_cast<RHI::VULKAN::VulkanDevice&>(device).getSwapChainClass();
    pipeDesc.colorAttachmentCount = 1;
    pipeDesc.colorFormats[0] = swapChain.GetSwapchainFormat();
    
    // Disable depth for simple triangle
    pipeDesc.depthTest = false;
    pipeDesc.depthWrite = false;
    pipeDesc.blendMode = RHI::BlendMode::None;
    
    m_Pipeline = device.CreateGraphicsPipeline(pipeDesc);
}

void TriangleTestLayer::OnDetach()
{
    auto& device = Renderer::GetDevice();
    device.DestroyGraphicsPipeline(m_Pipeline);
    device.DestroyShader(m_FragmentShader);
    device.DestroyShader(m_VertexShader);
    device.DestroyBuffer(m_VertexBuffer);
}

void TriangleTestLayer::OnUpdate(Timestep ts)
{
    auto& device = Renderer::GetDevice();
    auto& vulkanDevice = static_cast<RHI::VULKAN::VulkanDevice&>(device);
    auto& cmd = device.GetCurrentCommandBuffer();

    // First frame initialization of buffer data
    static bool uploaded = false;
    if (!uploaded) {
        std::vector<Vertex> vertices = {
            {{ 0.0f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},
            {{ 0.5f,  0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},
            {{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}}
        };
        cmd.UpdateBuffer(m_VertexBuffer, vertices.data(), vertices.size() * sizeof(Vertex), 0);
        uploaded = true;
    }

    device.Begin();

    auto& swapChainExtent = static_cast<RHI::VULKAN::VulkanDevice&>(device).getSwapChainClass().GetExtent();

    RHI::RenderPassDesc passDesc{};
    passDesc.renderWidth = swapChainExtent.width;
    passDesc.renderHeight = swapChainExtent.height;
    
    RHI::ColorAttachment colorAttachment{};
    // We can just use nullptr to indicate swapchain target for the demo or use the backend specific getter
    colorAttachment.texture = nullptr; 
    colorAttachment.clearColor = {0.1f, 0.1f, 0.1f, 1.0f};
    
    passDesc.colorAttachments[0] = colorAttachment;
    passDesc.colorAttachmentCount = 1;

    cmd.BeginRenderPass(passDesc);

    // Set viewport
    cmd.SetViewport(0.0f, 0.0f, (float)swapChainExtent.width, (float)swapChainExtent.height);
    cmd.SetScissor(0, 0, swapChainExtent.width, swapChainExtent.height);

    // Bind and draw (stubbed)
    cmd.BindPipeline(m_Pipeline);
    cmd.BindVertexBuffer(m_VertexBuffer, 0);
    cmd.Draw(3, 0);

    cmd.EndRenderPass();

    device.End();
}

void TriangleTestLayer::OnImGuiRender()
{
}

void TriangleTestLayer::OnEvent(Event& e)
{
}
