#include "VulkanTypes.h"
#include "UHE/Core/Log.h"
#include "UHE/RHI/RHITypes.h"
#include "UHE/Renderer/Shader.h"
#include "vulkan/vulkan.hpp"

namespace UHE::RHI::VULKAN
{
vk::Format MapTextureFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8_UNORM:
            return vk::Format::eR8G8B8A8Unorm;
        case TextureFormat::RGBA8_SRGB:
            return vk::Format::eR8G8B8A8Srgb;
        case TextureFormat::BGRA8_UNORM:
            return vk::Format::eB8G8R8A8Unorm;
        case TextureFormat::BGRA8_SRGB:
            return vk::Format::eB8G8R8A8Srgb;
        case TextureFormat::D24_UNORM_S8:
            return vk::Format::eD32SfloatS8Uint;
        case TextureFormat::D32_FLOAT:
            return vk::Format::eD32Sfloat;
        case TextureFormat::R32_SINT:
            return vk::Format::eR32Sint;
        // Declared in the RHI long before anything created one: a graph-owned
        // HDR target was the first, and reached vkCreateImage as UNDEFINED.
        case TextureFormat::R8_UNORM:
            return vk::Format::eR8Unorm;
        case TextureFormat::RG8_UNORM:
            return vk::Format::eR8G8Unorm;
        case TextureFormat::RGBA16F:
            return vk::Format::eR16G16B16A16Sfloat;
        case TextureFormat::RGBA32F:
            return vk::Format::eR32G32B32A32Sfloat;
        default:
            return vk::Format::eUndefined;
    }
}

vk::PrimitiveTopology MapTopology(PrimitiveTopology topology)
{
    switch (topology)
    {
        case PrimitiveTopology::TriangleList:
            return vk::PrimitiveTopology::eTriangleList;
        case PrimitiveTopology::TriangleStrip:
            return vk::PrimitiveTopology::eTriangleStrip;
        case PrimitiveTopology::LineList:
            return vk::PrimitiveTopology::eLineList;
        case PrimitiveTopology::PointList:
            return vk::PrimitiveTopology::ePointList;
    }
    return vk::PrimitiveTopology::eTriangleList;
}

vk::Format ShaderDataTypeToVulkanFormat(ShaderDataType type)
{
    switch (type)
    {
        case ShaderDataType::Float:
            return vk::Format::eR32Sfloat;
        case ShaderDataType::Float2:
            return vk::Format::eR32G32Sfloat;
        case ShaderDataType::Float3:
            return vk::Format::eR32G32B32Sfloat;
        case ShaderDataType::Float4:
            return vk::Format::eR32G32B32A32Sfloat;
        case ShaderDataType::Int:
            return vk::Format::eR32Sint;
        case ShaderDataType::Int2:
            return vk::Format::eR32G32Sint;
        case ShaderDataType::Int3:
            return vk::Format::eR32G32B32Sint;
        case ShaderDataType::Int4:
            return vk::Format::eR32G32B32A32Sint;
        case ShaderDataType::Mat3:
            return vk::Format::eR32G32B32Sfloat;
        case ShaderDataType::Mat4:
            return vk::Format::eR32G32B32A32Sfloat;
        case ShaderDataType::Bool:
            return vk::Format::eR32Uint;
        case ShaderDataType::None:
            return vk::Format::eUndefined;
    }
    return vk::Format::eUndefined;
}

vk::Format ToVkFormat(TextureFormat format)
{
    switch (format)
    {
        case TextureFormat::RGBA8_UNORM:
            return vk::Format::eR8G8B8A8Unorm;
        case TextureFormat::RGBA8_SRGB:
            return vk::Format::eR8G8B8A8Srgb;
        case TextureFormat::BGRA8_UNORM:
            return vk::Format::eB8G8R8A8Unorm;
        case TextureFormat::BGRA8_SRGB:
            return vk::Format::eB8G8R8A8Srgb;
        case TextureFormat::D24_UNORM_S8:
            return vk::Format::eD32SfloatS8Uint;
        case TextureFormat::D32_FLOAT:
            return vk::Format::eD32Sfloat;
        case TextureFormat::R32_SINT:
            return vk::Format::eR32Sint;
        default:
            return vk::Format::eUndefined;
    }
}

vk::SampleCountFlagBits ToVkSample(u32 sampleCount)
{
    switch (sampleCount)
    {
        case 1:
            return vk::SampleCountFlagBits::e1;
        case 2:
            return vk::SampleCountFlagBits::e2;
        case 4:
            return vk::SampleCountFlagBits::e4;
        case 8:
            return vk::SampleCountFlagBits::e8;
        case 16:
            return vk::SampleCountFlagBits::e16;
        case 32:
            return vk::SampleCountFlagBits::e32;
        case 64:
            return vk::SampleCountFlagBits::e64;
        default:
            return vk::SampleCountFlagBits::e1;
    }
}

vk::AttachmentLoadOp ToVkLoadOp(LoadOp loadOp)
{
    switch (loadOp)
    {
        case LoadOp::Load:
            return vk::AttachmentLoadOp::eLoad;
        case LoadOp::Clear:
            return vk::AttachmentLoadOp::eClear;
        case LoadOp::DontCare:
            return vk::AttachmentLoadOp::eDontCare;
        default:
            return vk::AttachmentLoadOp::eDontCare;
    }
}

vk::AttachmentStoreOp ToVkStoreOp(StoreOp storeOp)
{
    switch (storeOp)
    {
        case StoreOp::Store:
            return vk::AttachmentStoreOp::eStore;
        case StoreOp::DontCare:
            return vk::AttachmentStoreOp::eDontCare;
        default:
            return vk::AttachmentStoreOp::eDontCare;
    }
}

vk::ImageLayout ToVkImageLayout(TextureUsage usage)
{
    if (usage & TextureUsage::ColorAttach)
        return vk::ImageLayout::eColorAttachmentOptimal;
    else if (usage & TextureUsage::DepthAttach)
        return vk::ImageLayout::eDepthStencilAttachmentOptimal;
    else if (usage & TextureUsage::Sampled)
        return vk::ImageLayout::eShaderReadOnlyOptimal;
    else if (usage & TextureUsage::Storage)
        return vk::ImageLayout::eGeneral;
    else
        return vk::ImageLayout::eUndefined;
}

vk::DescriptorType ToVkDescriptorType(BufferUsageFlags usage)
{
    switch (usage)
    {
        case BufferUsageFlags::None:
            UHE_CORE_ASSERT(false, "Cannot convert BufferUsageFlags::None to a valid Vulkan Descriptor Type!");
            return vk::DescriptorType::eUniformBuffer;
        case BufferUsageFlags::Sampler:
            return vk::DescriptorType::eSampler;
        case BufferUsageFlags::CombinedImageSampler:
            return vk::DescriptorType::eCombinedImageSampler;
        case BufferUsageFlags::SampledImage:
            return vk::DescriptorType::eSampledImage;
        case BufferUsageFlags::StorageImage:
            return vk::DescriptorType::eStorageImage;
        case BufferUsageFlags::UniformTexelBuffer:
            return vk::DescriptorType::eUniformTexelBuffer;
        case BufferUsageFlags::StorageTexelBuffer:
            return vk::DescriptorType::eStorageTexelBuffer;
        case BufferUsageFlags::UniformBuffer:
            return vk::DescriptorType::eUniformBuffer;
        case BufferUsageFlags::StorageBuffer:
            return vk::DescriptorType::eStorageBuffer;
        case BufferUsageFlags::UniformBufferDynamic:
            return vk::DescriptorType::eUniformBufferDynamic;
        case BufferUsageFlags::StorageBufferDynamic:
            return vk::DescriptorType::eStorageBufferDynamic;
        case BufferUsageFlags::InputAttachment:
            return vk::DescriptorType::eInputAttachment;
        case BufferUsageFlags::InlineUniformBlock:
            return vk::DescriptorType::eInlineUniformBlock;
        case BufferUsageFlags::InlineUniformBlockEXT:
            return vk::DescriptorType::eInlineUniformBlockEXT;
        case BufferUsageFlags::AccelerationStructureKHR:
            return vk::DescriptorType::eAccelerationStructureKHR;
        case BufferUsageFlags::AccelerationStructureNV:
            return vk::DescriptorType::eAccelerationStructureNV;
#ifdef VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME
        case BufferUsageFlags::SampleWeightImageQCOM:
            return vk::DescriptorType::eSampleWeightImageQCOM;
        case BufferUsageFlags::BlockMatchImageQCOM:
            return vk::DescriptorType::eBlockMatchImageQCOM;
#endif
#if defined(VK_ENABLE_BETA_EXTENSIONS) || defined(VK_ARM_TENSOR_CORE_EXTENSION_NAME)
    // In some Vulkan SDKs, eTensorARM is only available if beta extensions are enabled, or in later versions.
    // Using a #if defined to avoid compilation error.
    #if VK_HEADER_VERSION >= 269
        case BufferUsageFlags::TensorARM:
            return vk::DescriptorType::eTensorARM;
    #endif
#endif
#ifdef VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME
        case BufferUsageFlags::MutableEXT:
            return vk::DescriptorType::eMutableEXT;
#endif
#ifdef VK_VALVE_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME
        case BufferUsageFlags::MutableVALVE:
            return vk::DescriptorType::eMutableVALVE;
#endif
#ifdef VK_NV_PARTITIONED_ACCELERATION_STRUCTURE_EXTENSION_NAME
        case BufferUsageFlags::PartitionedAccelerationStructureNV:
            return vk::DescriptorType::ePartitionedAccelerationStructureNV;
#endif
        default:
            UHE_CORE_ASSERT(false, "Invalid BufferUsageFlags!");
            return vk::DescriptorType::eUniformBuffer;
    }
}

vk::ImageLayout ToVkImageLayout(ImageState state)
{
    switch (state)
    {
        case ImageState::Undefined:
            return vk::ImageLayout::eUndefined;
        case ImageState::ColorAttachment:
            return vk::ImageLayout::eColorAttachmentOptimal;
        case ImageState::DepthAttachment:
            return vk::ImageLayout::eDepthStencilAttachmentOptimal;
        case ImageState::ShaderRead:
            return vk::ImageLayout::eShaderReadOnlyOptimal;
        case ImageState::TransferSrc:
            return vk::ImageLayout::eTransferSrcOptimal;
        case ImageState::TransferDst:
            return vk::ImageLayout::eTransferDstOptimal;
        case ImageState::Present:
            return vk::ImageLayout::ePresentSrcKHR;
        case ImageState::Storage:
            return vk::ImageLayout::eGeneral;
    }
    return vk::ImageLayout::eUndefined;
}

vk::PipelineStageFlags2 ToVkPipelineStage2(Stage stage)
{
    switch (stage)
    {
        case Stage::None:
            return vk::PipelineStageFlagBits2::eNone;
        case Stage::TopOfPipe:
            return vk::PipelineStageFlagBits2::eTopOfPipe;
        case Stage::Transfer:
            return vk::PipelineStageFlagBits2::eAllTransfer;
        case Stage::Compute:
            return vk::PipelineStageFlagBits2::eComputeShader;
        case Stage::Vertex:
            return vk::PipelineStageFlagBits2::eVertexShader;
        case Stage::Fragment:
            return vk::PipelineStageFlagBits2::eFragmentShader;
        case Stage::ColorOutput:
            return vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        case Stage::DepthEarly:
            return vk::PipelineStageFlagBits2::eEarlyFragmentTests;
        case Stage::DepthLate:
            return vk::PipelineStageFlagBits2::eLateFragmentTests;
        case Stage::BottomOfPipe:
            return vk::PipelineStageFlagBits2::eBottomOfPipe;
    }
    return vk::PipelineStageFlagBits2::eAllCommands;
}

vk::AccessFlags2 ToVkAccess2(Access access)
{
    switch (access)
    {
        case Access::None:
            return vk::AccessFlagBits2::eNone;
        case Access::TransferRead:
            return vk::AccessFlagBits2::eTransferRead;
        case Access::TransferWrite:
            return vk::AccessFlagBits2::eTransferWrite;
        case Access::ShaderRead:
            return vk::AccessFlagBits2::eShaderRead;
        case Access::ShaderWrite:
            return vk::AccessFlagBits2::eShaderWrite;
        // An attachment is read as well as written: LoadOp::Load and blending
        // read colour, the depth test reads depth. Without the read bit a
        // barrier makes earlier writes available but not visible to those reads.
        case Access::ColorWrite:
            return vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eColorAttachmentRead;
        case Access::DepthWrite:
            return vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eDepthStencilAttachmentRead;
        case Access::MemoryRead:
            return vk::AccessFlagBits2::eMemoryRead;
        case Access::MemoryWrite:
            return vk::AccessFlagBits2::eMemoryWrite;
    }
    return vk::AccessFlagBits2::eNone;
}

// Legacy mappings. Lossy by nature: 64-bit stages/access collapse to the nearest
// v1 bit, and the barrier encoder coalesces stages across a barrier group.
vk::PipelineStageFlags ToVkPipelineStage1(Stage stage)
{
    switch (stage)
    {
        case Stage::None:
            return {};
        case Stage::TopOfPipe:
            return vk::PipelineStageFlagBits::eTopOfPipe;
        case Stage::Transfer:
            return vk::PipelineStageFlagBits::eTransfer;
        case Stage::Compute:
            return vk::PipelineStageFlagBits::eComputeShader;
        case Stage::Vertex:
            return vk::PipelineStageFlagBits::eVertexShader;
        case Stage::Fragment:
            return vk::PipelineStageFlagBits::eFragmentShader;
        case Stage::ColorOutput:
            return vk::PipelineStageFlagBits::eColorAttachmentOutput;
        case Stage::DepthEarly:
            return vk::PipelineStageFlagBits::eEarlyFragmentTests;
        case Stage::DepthLate:
            return vk::PipelineStageFlagBits::eLateFragmentTests;
        case Stage::BottomOfPipe:
            return vk::PipelineStageFlagBits::eBottomOfPipe;
    }
    return vk::PipelineStageFlagBits::eAllCommands;
}

vk::AccessFlags ToVkAccess1(Access access)
{
    switch (access)
    {
        case Access::None:
            return {};
        case Access::TransferRead:
            return vk::AccessFlagBits::eTransferRead;
        case Access::TransferWrite:
            return vk::AccessFlagBits::eTransferWrite;
        case Access::ShaderRead:
            return vk::AccessFlagBits::eShaderRead;
        case Access::ShaderWrite:
            return vk::AccessFlagBits::eShaderWrite;
        case Access::ColorWrite:
            return vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead;
        case Access::DepthWrite:
            return vk::AccessFlagBits::eDepthStencilAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentRead;
        case Access::MemoryRead:
            return vk::AccessFlagBits::eMemoryRead;
        case Access::MemoryWrite:
            return vk::AccessFlagBits::eMemoryWrite;
    }
    return {};
}

} // namespace UHE::RHI::VULKAN
