#pragma once
/**
 * \file RHITypes.h
 * \brief Backend-agnostic descriptors, enums and handles shared by every RHI backend.
 *
 * This header is intentionally free of any graphics-API types: it is the vocabulary
 * the engine and the backends agree on. Vulkan specifics live in
 * Platform/Vulkan/VulkanTypes.h, which maps these values onto vk:: enums.
 *
 * \see https://docs.vulkan.org/spec/latest/
 */
#include <array>
#include <glm/glm.hpp>
#include <initializer_list>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE::RHI
{

// ─── Backend selection ───────────────────────────────────────────

/// Graphics API backing an RHIDevice instance.
enum class Backend : u8
{
    None = 0,
    Vulkan,
    DX12,
    Metal
};

// ─── Opaque handles ──────────────────────────────────────────────
// Pointer-to-incomplete-type idioms keep API detail out of the public headers and
// give a tiny bit of type safety (BufferHandle and TextureHandle are distinct).

using BufferHandle = struct BufferHandle_T*;
using TextureHandle = struct TextureHandle_T*;
using PipelineHandle = struct PipelineHandle_T*;
using ShaderHandle = struct ShaderHandle_T*;
using DescriptorHandle = struct DescriptorHandle_T*;

// ─── Enums ──────────────────────────────────────────────────────

/// Coarse intent for a buffer slot; the backend turns it into usage flags.
enum class BufferUsage : u8
{
    Vertex,
    Index,
    Uniform,
    Storage,
    Staging
};

/**
 * \brief Descriptor-type bitmask — one bit per role a resource can play.
 * \note  Values mirror VkDescriptorType so the Vulkan backend maps them 1:1.
 * \see   https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorType.html
 */
enum class BufferUsageFlags : u32
{
    None = 0,
    Sampler = 1 << 0,
    CombinedImageSampler = 1 << 1,
    SampledImage = 1 << 2,
    StorageImage = 1 << 3,
    UniformTexelBuffer = 1 << 4,
    StorageTexelBuffer = 1 << 5,
    UniformBuffer = 1 << 6,
    StorageBuffer = 1 << 7,
    UniformBufferDynamic = 1 << 8,
    StorageBufferDynamic = 1 << 9,
    InputAttachment = 1 << 10,
    InlineUniformBlock = 1 << 11,
    InlineUniformBlockEXT = 1 << 12,
    AccelerationStructureKHR = 1 << 13,
    AccelerationStructureNV = 1 << 14,
    SampleWeightImageQCOM = 1 << 15,
    BlockMatchImageQCOM = 1 << 16,
    TensorARM = 1 << 17,
    MutableEXT = 1 << 18,
    MutableVALVE = 1 << 19,
    PartitionedAccelerationStructureNV = 1 << 20
};
inline BufferUsageFlags operator|(BufferUsageFlags a, BufferUsageFlags b)
{
    return static_cast<BufferUsageFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline bool operator&(BufferUsageFlags a, BufferUsageFlags b)
{
    return (static_cast<u32>(a) & static_cast<u32>(b)) != 0;
}

/// Pixel/texel format. The Vulkan backend maps these to vk::Format.
enum class TextureFormat : u8
{
    Undefined = 0,
    RGBA8_UNORM,
    RGBA8_SRGB,
    BGRA8_UNORM,
    BGRA8_SRGB,
    R8_UNORM,
    RG8_UNORM,
    RGBA16F,
    RGBA32F,
    R32_SINT,     // for entity ID / pick buffer
    D24_UNORM_S8, // depth-stencil
    D32_FLOAT,    // depth only
};

/// How a texture is used across the frame (sampled, attachment, storage, copy source/sink).
enum class TextureUsage : u32
{
    Sampled = 1 << 0,
    ColorAttach = 1 << 1,
    DepthAttach = 1 << 2,
    Storage = 1 << 3,
    TransferSrc = 1 << 4,
    TransferDst = 1 << 5,
    None = 0
};
inline TextureUsage operator|(TextureUsage a, TextureUsage b)
{
    return static_cast<TextureUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline bool operator&(TextureUsage a, TextureUsage b)
{
    return (static_cast<u32>(a) & static_cast<u32>(b)) != 0;
}

/// Programmable pipeline stage a shader or push range binds to.
enum class ShaderStage : u8
{
    Vertex,
    Fragment,
    Compute,
    AllGraphics
};

/// Attachment load behaviour (keep vs. clear vs. leave undefined).
enum class LoadOp : u8
{
    Load,
    Clear,
    DontCare
};
/// Attachment store behaviour (write back vs. discard).
enum class StoreOp : u8
{
    Store,
    DontCare
};

/**
 * \brief How vertices are assembled into primitives.
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkPrimitiveTopology.html
 */
enum class PrimitiveTopology : u8
{
    TriangleList,
    TriangleStrip,
    LineList,
    PointList
};

/// Colour blend preset for a graphics pipeline.
enum class BlendMode : u8
{
    None,
    Alpha,
    Additive
};

// ─── Descriptors ────────────────────────────────────────────────

/// Parameters for RHIDevice::CreateBuffer.
struct BufferDesc
{
    u64 size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    bool hostVisible = false; // CPU-mappable (staging / uniform)
    const char* debugName = nullptr;
};

/// Parameters for RHIDevice::CreateTexture.
struct TextureDesc
{
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;
    TextureFormat format = TextureFormat::RGBA8_SRGB;
    TextureUsage usage = TextureUsage::Sampled;
    const char* debugName = nullptr;
};

// ─── Buffer Layout ──────────────────────────────────────────────

/// Vertex-attribute component type used by BufferLayout.
enum class ShaderDataType
{
    None = 0,
    Float,
    Float2,
    Float3,
    Float4,
    Mat3,
    Mat4,
    Int,
    Int2,
    Int3,
    Int4,
    Bool
};

inline u32 ShaderDataTypeSize(ShaderDataType type)
{
    switch (type)
    {
        case ShaderDataType::Float:
            return 4;
        case ShaderDataType::Float2:
            return 4 * 2;
        case ShaderDataType::Float3:
            return 4 * 3;
        case ShaderDataType::Float4:
            return 4 * 4;
        case ShaderDataType::Mat3:
            return 4 * 3 * 3;
        case ShaderDataType::Mat4:
            return 4 * 4 * 4;
        case ShaderDataType::Int:
            return 4;
        case ShaderDataType::Int2:
            return 4 * 2;
        case ShaderDataType::Int3:
            return 4 * 3;
        case ShaderDataType::Int4:
            return 4 * 4;
        case ShaderDataType::Bool:
            return 1;
    }
    return 0;
}

/// One vertex attribute: name, type, byte size and offset within a vertex.
struct BufferElement
{
    std::string Name;
    ShaderDataType Type = ShaderDataType::None;
    u32 Size = 0;
    u32 Offset = 0;
    bool Normalized = false;

    BufferElement() = default;
    BufferElement(ShaderDataType type, const std::string& name, bool normalized = false)
        : Name(name), Type(type), Size(ShaderDataTypeSize(type)), Offset(0), Normalized(normalized)
    {
    }

    u32 GetComponentCount() const
    {
        switch (Type)
        {
            case ShaderDataType::Float:
                return 1;
            case ShaderDataType::Float2:
                return 2;
            case ShaderDataType::Float3:
                return 3;
            case ShaderDataType::Float4:
                return 4;
            case ShaderDataType::Mat3:
                return 3 * 3;
            case ShaderDataType::Mat4:
                return 4 * 4;
            case ShaderDataType::Int:
                return 1;
            case ShaderDataType::Int2:
                return 2;
            case ShaderDataType::Int3:
                return 3;
            case ShaderDataType::Int4:
                return 4;
            case ShaderDataType::Bool:
                return 1;
        }
        return 0;
    }
};

/// Ordered set of vertex attributes; computes per-element offsets and the vertex stride.
class BufferLayout
{
public:
    BufferLayout() = default;
    BufferLayout(const std::initializer_list<BufferElement>& elements) : m_Elements(elements)
    {
        CalculateOffsetsAndStride();
    }
    inline u32 GetStride() const { return m_Stride; }
    inline const std::vector<BufferElement>& GetElements() const { return m_Elements; }

    std::vector<BufferElement>::iterator begin() { return m_Elements.begin(); }
    std::vector<BufferElement>::iterator end() { return m_Elements.end(); }
    std::vector<BufferElement>::const_iterator begin() const { return m_Elements.begin(); }
    std::vector<BufferElement>::const_iterator end() const { return m_Elements.end(); }

private:
    void CalculateOffsetsAndStride()
    {
        u32 offset = 0;
        m_Stride = 0;
        for (auto& element : m_Elements)
        {
            element.Offset = offset;
            offset += element.Size;
            m_Stride += element.Size;
        }
    }

private:
    std::vector<BufferElement> m_Elements;
    u32 m_Stride = 0;
};

// ─── Descriptors ────────────────────────────────────────────────

/// SPIR-V blob plus the stage it belongs to.
struct ShaderDesc
{
    ShaderStage stage = ShaderStage::Vertex;
    const u8* spirvData = nullptr;
    u64 spirvSize = 0;
    const char* entryPoint = "main";
    const char* debugName = nullptr;
};

/// Attachment wiring for one subpass (which colour/depth targets it reads and writes).
struct SubpassDesc
{
    u32 colorAttachmentCount = 0;
    u32 colorAttachments[8] = {};
    u32 depthAttachment = 0;
};

/// A single colour target with its clear/store policy and layout requirements.
struct ColorAttachment
{
    TextureHandle texture = nullptr;
    LoadOp loadOp = LoadOp::Clear;
    StoreOp storeOp = StoreOp::Store;
    glm::vec4 clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    TextureFormat format = TextureFormat::BGRA8_SRGB;
    TextureUsage initialusage = TextureUsage::None; // Bitmask of TextureUsage flags
    TextureUsage finalusage = TextureUsage::None;   // Bitmask of TextureUsage flags
    u32 sampleCount = 1;
    LoadOp stencilLoadOp = LoadOp::DontCare;
    StoreOp stencilStoreOp = StoreOp::DontCare;
};

/// Optional depth/stencil target with its clear values.
struct DepthAttachment
{
    TextureHandle texture = nullptr;
    LoadOp loadOp = LoadOp::Clear;
    StoreOp storeOp = StoreOp::Store;
    float clearDepth = 1.0f;
    u8 clearStencil = 0;
};

/// Everything needed to begin a render pass: targets, subpasses and output extent.
struct RenderPassDesc
{
    ColorAttachment colorAttachments[8] = {};
    u32 colorAttachmentCount = 0;
    u32 subpassCount = 1;
    SubpassDesc subpasses[8] = {};
    u32 dependencyCount = 0;
    DepthAttachment depthAttachment = {};
    bool hasDepth = false;
    u32 renderWidth = 0;
    u32 renderHeight = 0;
};

/// Parameters for RHIDevice::CreateGraphicsPipeline.
struct GraphicsPipelineDesc
{
    ShaderHandle vertexShader = nullptr;
    ShaderHandle fragmentShader = nullptr;
    BufferLayout vertexLayout; // reuse existing engine type
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    std::array<TextureFormat, 8> colorFormats = {TextureFormat::BGRA8_SRGB};
    u32 colorAttachmentCount = 1;
    TextureFormat depthFormat = TextureFormat::D24_UNORM_S8;
    BlendMode blendMode = BlendMode::None;
    bool depthTest = true;
    bool depthWrite = true;
    u32 pushConstantSize = 0;
    const char* debugName = nullptr;
    RenderPassDesc renderPassDesc = {};
};

/// Parameters for RHIDevice::CreateComputePipeline.
struct ComputePipelineDesc
{
    ShaderHandle computeShader = nullptr;
    u32 pushConstantSize = 0;
    const char* debugName = nullptr;
};

// ─── Swapchain Info ─────────────────────────────────────────────

/// Native window handle plus the initial client size for the swapchain.
struct SwapchainDesc
{
    void* nativeWindow = nullptr;
    u32 width = 1920;
    u32 height = 1080;
    bool vsync = true;
};

} // namespace UHE::RHI
