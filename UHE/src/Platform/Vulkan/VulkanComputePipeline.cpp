#include "uhepch.h"
#include "VulkanComputePipeline.h"
#include "VulkanCoreObjects.h"
#include "VulkanDescriptorManager.h"
#include "VulkanLogicalDevice.h"
#include "VulkanShader.h"

namespace UHE::RHI::VULKAN
{

void VulkanComputePipeline::CreateComputePipeline(VulkanLogicalDevice& Device,
                                                  VulkanDescriptorManager& descriptorManager,
                                                  const ComputePipelineDesc& desc)
{
    auto* shader = reinterpret_cast<VulkanShader*>(desc.computeShader);
    UHE_CORE_ASSERT(shader != nullptr, "ComputePipelineDesc::computeShader is null!");
    UHE_CORE_ASSERT(shader->GetStage() == ShaderStage::Compute, "VulkanComputePipeline needs a compute shader!");

    const vk::PipelineShaderStageCreateInfo shaderStage{.flags = {},
                                                        .stage = vk::ShaderStageFlagBits::eCompute,
                                                        .module = shader->GetModule(),
                                                        .pName = shader->GetEntryPoint()};

    auto globalLayout = descriptorManager.GetLayoutHandle();

    vk::PushConstantRange pushConstantRange{};
    // Set 0 is the bindless set every pipeline has; sets 1..3 are the desc's own (RHICore.h).
    const std::vector<vk::DescriptorSetLayout> setLayouts = PipelineSetLayouts(
        globalLayout,
        std::span(desc.descriptorSetLayouts.data(),
                  std::min<std::size_t>(desc.descriptorSetLayoutCount, desc.descriptorSetLayouts.size())));
    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{.flags = {},
                                                    .setLayoutCount = static_cast<u32>(setLayouts.size()),
                                                    .pSetLayouts = setLayouts.data(),
                                                    .pushConstantRangeCount = 0,
                                                    .pPushConstantRanges = nullptr};

    if (desc.pushConstantSize > 0)
    {
        pushConstantRange = vk::PushConstantRange{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                                  .offset = 0,
                                                  .size = desc.pushConstantSize};
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    }

    m_PipelineLayout = vk::raii::PipelineLayout(Device.getLogicalDevice(), pipelineLayoutInfo);

    const vk::ComputePipelineCreateInfo pipelineInfo{
        .flags = {},
        .stage = shaderStage,
        .layout = *m_PipelineLayout,
    };

    m_ComputePipeline = vk::raii::Pipeline(Device.getLogicalDevice(), nullptr, pipelineInfo);
}

void VulkanComputePipeline::ShutDownComputePipeline()
{
    m_ComputePipeline = nullptr;
    m_PipelineLayout = nullptr;
}

} // namespace UHE::RHI::VULKAN
