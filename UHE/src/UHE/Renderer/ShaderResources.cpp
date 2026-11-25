#include "uhepch.h"
#include "ShaderResources.h"
#include <algorithm>

namespace UHE
{
namespace
{
const char* StageLabel(RHI::ShaderStage stage)
{
    switch (stage)
    {
        case RHI::ShaderStage::Vertex:
            return "vertex";
        case RHI::ShaderStage::Fragment:
            return "fragment";
        case RHI::ShaderStage::Compute:
            return "compute";
        default:
            return "graphics";
    }
}
} // namespace

RHI::ShaderHandle CreateStageShader(RHI::RHIDevice& device, const ShaderCache::Shader& shader, RHI::ShaderStage stage,
                                    const char* debugName)
{
    const ShaderStageBinary* binary = shader.FindStage(stage);
    if (binary == nullptr || binary->SPIRV.empty())
    {
        UHE_CORE_ERROR("Shader '{0}' has no {1} stage", shader.SourcePath, StageLabel(stage));
        return nullptr;
    }

    RHI::ShaderDesc desc{};
    desc.stage = stage;
    desc.spirvData = binary->SPIRV.data();
    desc.spirvSize = binary->SPIRV.size();
    desc.debugName = debugName;
    return device.CreateShader(desc);
}

RHI::BufferLayout ReflectedVertexLayout(const ShaderCache::Shader& shader)
{
    RHI::BufferLayout layout;

    const ShaderStageBinary* vertex = shader.FindStage(RHI::ShaderStage::Vertex);
    if (vertex == nullptr)
    {
        return layout;
    }

    for (const ShaderVertexInput& input : vertex->Reflection.VertexInputs)
    {
        if (input.Type == RHI::ShaderDataType::None)
        {
            UHE_CORE_WARN("Shader '{0}': vertex attribute '{1}' at location {2} has no vertex format",
                          shader.SourcePath, input.Name, input.Location);
            continue;
        }
        layout.PushElement({input.Type, input.Name});
    }
    return layout;
}

u32 ReflectedPushConstantSize(const ShaderCache::Shader& shader)
{
    u32 size = 0;
    for (const ShaderStageBinary& binary : shader.Stages)
    {
        size = std::max(size, binary.Reflection.PushConstantSize);
    }
    return size;
}
} // namespace UHE
