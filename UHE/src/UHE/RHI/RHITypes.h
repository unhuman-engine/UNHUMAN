#pragma once
#include <array>
#include <glm/glm.hpp>
#include <initializer_list>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE::RHI
{

// ─── Backend Selection ───────────────────────────────────────────
enum class Backend : u8
{
    None = 0,
    Vulkan,
    DX12,
    Metal
};

// ─── Opaque Handles ─────────────────────────────────────────────

using BufferHandle = struct BufferHandle_T*;
using TextureHandle = struct TextureHandle_T*;
using PipelineHandle = struct PipelineHandle_T*;
using ShaderHandle = struct ShaderHandle_T*;
using DescriptorHandle = struct DescriptorHandle_T*;

// ─── Enums ──────────────────────────────────────────────────────

enum class BufferUsage : u8
{
    Vertex,
    Index,
    Uniform,
    Storage,
    Staging
};

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

enum class ShaderStage : u8
{
    Vertex,
    Fragment,
    Compute,
    AllGraphics
};

enum class LoadOp : u8
{
    Load,
    Clear,
    DontCare
};
enum class StoreOp : u8
{
    Store,
    DontCare
};

enum class PrimitiveTopology : u8
{
    TriangleList,
    TriangleStrip,
    LineList,
    PointList
};

enum class BlendMode : u8
{
    None,
    Alpha,
    Additive
};

// Face culling for the rasterizer. Vulkan bakes this into
// VkPipelineRasterizationStateCreateInfo rather than exposing it as dynamic
// state, so a material with doubleSided=true needs a separate pipeline variant
// selected at draw time - it cannot be switched per draw.
enum class CullMode : u8
{
    None,        // render both faces
    Back,        // cull back faces (the usual default)
    Front,       // cull front faces
};

// ─── Descriptors ────────────────────────────────────────────────

struct BufferDesc
{
    u64 size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    bool hostVisible = false; // CPU-mappable (staging / uniform)
    const char* debugName = nullptr;
};

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

struct ShaderDesc
{
    ShaderStage stage = ShaderStage::Vertex;
    const u8* spirvData = nullptr;
    u64 spirvSize = 0;
    const char* entryPoint = "main";
    const char* debugName = nullptr;
};

struct SubpassDesc
{
    u32 colorAttachmentCount = 0;
    u32 colorAttachments[8] = {};
    u32 depthAttachment = 0;
};

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

struct DepthAttachment
{
    TextureHandle texture = nullptr;
    LoadOp loadOp = LoadOp::Clear;
    StoreOp storeOp = StoreOp::Store;
    float clearDepth = 1.0f;
    u8 clearStencil = 0;
};

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
    // Default None, not Back: culling was previously hardcoded to none for every
    // pipeline, so defaulting to Back would silently change how existing content
    // renders. Each pipeline opts in deliberately.
    CullMode cullMode = CullMode::None;
    bool depthTest = true;
    bool depthWrite = true;
    u32 pushConstantSize = 0;
    const char* debugName = nullptr;
    RenderPassDesc renderPassDesc = {};
};

struct ComputePipelineDesc
{
    ShaderHandle computeShader = nullptr;
    u32 pushConstantSize = 0;
    const char* debugName = nullptr;
};

// ─── Swapchain Info ─────────────────────────────────────────────

struct SwapchainDesc
{
    void* nativeWindow = nullptr;
    u32 width = 1920;
    u32 height = 1080;
    bool vsync = true;
};

} // namespace UHE::RHI
