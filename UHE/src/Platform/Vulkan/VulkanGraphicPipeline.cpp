#include "uhepch.h" // pragma
#include "VulkanGraphicPipeline.h"
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/VulkanFramebuffer.h"
#include "UHE/RHI/RHIDevice.h"
#include "UHE/RHI/RHITypes.h"
#include "VulkanDescriptorManager.h"
#include "VulkanExtensionCheck.h"
#include "VulkanLogicalDevice.h"
#include "VulkanRenderPass.h"
#include "VulkanShader.h"
#include "VulkanTypes.h"
#include "vulkan/vulkan.hpp"

namespace UHE::RHI::VULKAN
{

VulkanGraphicPipeline::VulkanGraphicPipeline() = default;
VulkanGraphicPipeline::~VulkanGraphicPipeline() = default;

void VulkanGraphicPipeline::Init() {}

void VulkanGraphicPipeline::Bind() {}

void VulkanGraphicPipeline::createGraphicsPipeline(VulkanLogicalDevice& Device,
                                                   VulkanDescriptorManager& descriptorManager, const VulkanContext& ctx,
                                                   const GraphicsPipelineDesc& desc)
{

    const auto& check = ctx.CheckExtensions;
    auto vertModule = reinterpret_cast<VulkanShader*>(desc.vertexShader)->GetModule();
    auto fragModule = reinterpret_cast<VulkanShader*>(desc.fragmentShader)->GetModule();

    vk::PipelineShaderStageCreateInfo shaderStages[] = {
        {.flags = {}, .stage = vk::ShaderStageFlagBits::eVertex, .module = vertModule, .pName = "main"},
        {.flags = {}, .stage = vk::ShaderStageFlagBits::eFragment, .module = fragModule, .pName = "main"}};

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo = CreateVertexInputState(desc.vertexLayout);

    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{
        .flags = {}, .topology = MapTopology(desc.topology), .primitiveRestartEnable = vk::False};

    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .flags = {},
        .depthClampEnable = vk::False,
        .rasterizerDiscardEnable = vk::False,
        .polygonMode = vk::PolygonMode::eFill, // can add Wireframe configuration if supported by desc
        // Was hardcoded eNone. Now driven by desc.cullMode, whose default is
        // also None so every existing pipeline renders exactly as before -
        // opting in to culling is left to each pipeline's owner.
        .cullMode = desc.cullMode == CullMode::None     ? vk::CullModeFlagBits::eNone
                    : desc.cullMode == CullMode::Front  ? vk::CullModeFlagBits::eFront
                                                         : vk::CullModeFlagBits::eBack,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = vk::False,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f};

    auto globalLayout = descriptorManager.GetLayoutHandle();
    vk::PushConstantRange pushConstantRange{};
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{.flags = {},
                                                    .setLayoutCount = 1,
                                                    .pSetLayouts = &globalLayout,
                                                    .pushConstantRangeCount = 0,
                                                    .pPushConstantRanges = nullptr};

    if (desc.pushConstantSize > 0)
    {
        pushConstantRange = vk::PushConstantRange{
            .stageFlags = vk::ShaderStageFlagBits::eAllGraphics, .offset = 0, .size = desc.pushConstantSize};
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    }

    m_PipelineLayout = vk::raii::PipelineLayout(Device.getLogicalDevice(), pipelineLayoutInfo);

    vk::PipelineMultisampleStateCreateInfo multisampling{.flags = {},
                                                         .rasterizationSamples = vk::SampleCountFlagBits::e1,
                                                         .sampleShadingEnable = vk::False,
                                                         .minSampleShading = 1.0f,
                                                         .pSampleMask = nullptr,
                                                         .alphaToCoverageEnable = vk::False,
                                                         .alphaToOneEnable = vk::False};

    vk::PipelineColorBlendAttachmentState colorBlendAttachment{
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = vk::BlendFactor::eZero,
        .dstColorBlendFactor = vk::BlendFactor::eZero,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eZero,
        .dstAlphaBlendFactor = vk::BlendFactor::eZero,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};

    if (desc.blendMode == BlendMode::Alpha)
    {
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
        colorBlendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        colorBlendAttachment.colorBlendOp = vk::BlendOp::eAdd;
        colorBlendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        colorBlendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eZero;
        colorBlendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
    }
    else if (desc.blendMode == BlendMode::Additive)
    {
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = vk::BlendFactor::eOne;
        colorBlendAttachment.dstColorBlendFactor = vk::BlendFactor::eOne;
        colorBlendAttachment.colorBlendOp = vk::BlendOp::eAdd;
        colorBlendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        colorBlendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOne;
        colorBlendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
    }

    std::vector<vk::PipelineColorBlendAttachmentState> colorBlendAttachments(desc.colorAttachmentCount,
                                                                             colorBlendAttachment);
    for (u32 i = 0; i < desc.colorAttachmentCount; i++)
    {
        if (desc.colorFormats[i] == TextureFormat::R32_SINT)
        {
            colorBlendAttachments[i].blendEnable = VK_FALSE;
        }
    }

    vk::PipelineColorBlendStateCreateInfo colorBlending{.flags = {},
                                                        .logicOpEnable = VK_FALSE,
                                                        .logicOp = vk::LogicOp::eCopy,
                                                        .attachmentCount =
                                                            static_cast<uint32_t>(colorBlendAttachments.size()),
                                                        .pAttachments = colorBlendAttachments.data()};

    vk::PipelineDepthStencilStateCreateInfo depthStencil{.flags = {},
                                                         .depthTestEnable = desc.depthTest ? vk::True : vk::False,
                                                         .depthWriteEnable = desc.depthWrite ? vk::True : vk::False,
                                                         .depthCompareOp = vk::CompareOp::eLessOrEqual,
                                                         .depthBoundsTestEnable = vk::False,
                                                         .stencilTestEnable = vk::False};

    std::vector<vk::DynamicState> dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamicStateCreateInfo{.flags = {},
                                                              .dynamicStateCount =
                                                                  static_cast<uint32_t>(dynamicStates.size()),
                                                              .pDynamicStates = dynamicStates.data()};

    vk::PipelineViewportStateCreateInfo viewportState{.flags = {}, .viewportCount = 1, .scissorCount = 1};

    std::vector<vk::Format> colorFormats;
    colorFormats.reserve(desc.colorAttachmentCount);
    for (u32 i = 0; i < desc.colorAttachmentCount; i++)
    {
        colorFormats.emplace_back(MapTextureFormat(desc.colorFormats[i]));
    }
    vk::Format depthFormat = MapTextureFormat(desc.depthFormat);

    vk::PipelineRenderingCreateInfo pipelineRenderCreateInfo{
        .viewMask = 0,
        .colorAttachmentCount = static_cast<uint32_t>(colorFormats.size()),
        .pColorAttachmentFormats = colorFormats.data(),
        .depthAttachmentFormat = (desc.depthTest || desc.depthWrite) ? depthFormat : vk::Format::eUndefined,
        .stencilAttachmentFormat = (desc.depthTest || desc.depthWrite) ? depthFormat : vk::Format::eUndefined};

    vk::GraphicsPipelineCreateInfo pipelineInfo{
        .pNext = &pipelineRenderCreateInfo,
        .flags = {},
        .stageCount = 2,
        .pStages = shaderStages, // Changed from pStage to pStages
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pTessellationState = nullptr,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlending,
        .pDynamicState = &dynamicStateCreateInfo,
        .layout = *m_PipelineLayout, // Extracts the raw vk::PipelineLayout handle
        .renderPass = nullptr        // Correct for Dynamic Rendering
    };
    if (!check->Supports(Extension::DynamicRendering))
    {
        m_FallbackRenderPass.Init(ctx, desc.renderPassDesc);
        pipelineInfo.renderPass = m_FallbackRenderPass.GetRenderPass();
        pipelineInfo.pNext = nullptr;
    }

    m_GraphicsPipeline = vk::raii::Pipeline(Device.getLogicalDevice(), nullptr, pipelineInfo);
}

vk::PipelineVertexInputStateCreateInfo VulkanGraphicPipeline::CreateVertexInputState(const BufferLayout& layout)
{
    m_BindingDescription.clear();
    m_AttributeDescription.clear();

    if (layout.GetElements().empty())
    {
        return vk::PipelineVertexInputStateCreateInfo{.flags = {}};
    }

    vk::VertexInputBindingDescription bindingDesc{
        .binding = 0, .stride = layout.GetStride(), .inputRate = vk::VertexInputRate::eVertex};
    m_BindingDescription.push_back(bindingDesc);

    u32 location = 0;

    for (const auto& elements : layout.GetElements())
    {
        u32 slotcount = 1;
        u32 slotsize = 0;

        if (elements.Type == ShaderDataType::Mat3)
        {
            slotcount = 3;
            slotsize = 4 * 3;
        }
        if (elements.Type == ShaderDataType::Mat4)
        {
            slotcount = 4;
            slotsize = 4 * 4;
        }

        for (u32 i = 0; i < slotcount; i++)
        {
            vk::VertexInputAttributeDescription attributeDsec{.location = location,
                                                              .binding = 0,
                                                              .format = ShaderDataTypeToVulkanFormat(elements.Type),
                                                              .offset = elements.Offset + (i * slotsize)};
            m_AttributeDescription.push_back(attributeDsec);
            location++;
        }
    }

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo{
        .flags = {},
        .vertexBindingDescriptionCount = static_cast<u32>(m_BindingDescription.size()),
        .pVertexBindingDescriptions = m_BindingDescription.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(m_AttributeDescription.size()),
        .pVertexAttributeDescriptions = m_AttributeDescription.data()};

    return vertexInputInfo;
};

} // namespace UHE::RHI::VULKAN
