#pragma once
/**
 * @file RHICore.h
 * @brief The RHI's thin core (issue #30): explicit barriers, copies, queries, command lists and native handles.
 *
 * The convenience surface (BeginRenderPass, BindPipeline, Draw, ...) places
 * barriers and binds the bindless set for the caller. The core does nothing
 * implicitly: RHICommandBuffer::Barrier is the only thing that changes a
 * resource's state, BeginRendering opens a scope over attachments already in
 * their attachment state, and the caller submits command lists it began. The
 * render graph (UHE/RHI/RenderGraph) is written against this vocabulary.
 *
 * Every type here is backend-neutral; Platform/Vulkan translates them.
 */
#include <array>
#include <span>
#include <string>
#include <vector>
#include "UHE/RHI/RHIBarrier.h"
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI
{

class RHICommandBuffer;

using QueryPoolHandle = struct QueryPoolHandle_T*;
using DescriptorSetHandle = struct DescriptorSetHandle_T*;

/// Every mip level, or every array layer, from the base one on.
inline constexpr u32 kRemaining = ~0u;
/// A buffer range up to the end of the buffer.
inline constexpr u64 kWholeSize = ~0ull;

// ── Barriers ────────────────────────────────────────────────────────────────

/// One texture transition: what wrote it and how, what reads it next, and its layout before and after.
struct TextureBarrier
{
    TextureHandle Texture = nullptr;
    ImageState Old = ImageState::Undefined; ///< Undefined discards the contents
    ImageState New = ImageState::Undefined;
    Stage SrcStage = Stage::AllCommands;
    Stage DstStage = Stage::AllCommands;
    Access SrcAccess = Access::MemoryWrite;
    Access DstAccess = Access::MemoryRead;
    u32 BaseMip = 0;
    u32 MipCount = kRemaining;
    u32 BaseLayer = 0;
    u32 LayerCount = kRemaining;
};

/// One buffer range dependency.
struct BufferBarrier
{
    BufferHandle Buffer = nullptr;
    u64 Offset = 0;
    u64 Size = kWholeSize;
    Stage SrcStage = Stage::AllCommands;
    Stage DstStage = Stage::AllCommands;
    Access SrcAccess = Access::MemoryWrite;
    Access DstAccess = Access::MemoryRead;
};

/// A dependency on all memory, with no resource named.
struct MemoryDependency
{
    Stage SrcStage = Stage::AllCommands;
    Stage DstStage = Stage::AllCommands;
    Access SrcAccess = Access::MemoryWrite;
    Access DstAccess = Access::MemoryRead;
};

// ── Copies ──────────────────────────────────────────────────────────────────

struct BufferCopy
{
    u64 SrcOffset = 0;
    u64 DstOffset = 0;
    u64 Size = 0;
};

/// A rectangle of one mip level of a texture. Width/Height 0: the whole level.
struct TextureRegion
{
    u32 Mip = 0;
    u32 BaseLayer = 0;
    u32 LayerCount = 1;
    i32 X = 0;
    i32 Y = 0;
    u32 Width = 0;
    u32 Height = 0;
};

/// Buffer bytes into a texture region (or back). Rows are tightly packed unless RowLength says otherwise.
struct BufferTextureCopy
{
    u64 BufferOffset = 0;
    u32 RowLength = 0; ///< in texels; 0: the region's width
    TextureRegion Region;
};

/// Texels from one region to another of the same size.
struct TextureCopy
{
    TextureRegion Src;
    TextureRegion Dst;
};

/// A scaled copy: Src's rectangle stretched onto Dst's.
struct TextureBlit
{
    TextureRegion Src;
    TextureRegion Dst;
};

enum class BlitFilter : u8
{
    Nearest,
    Linear,
};

// ── Queries ─────────────────────────────────────────────────────────────────

enum class QueryType : u8
{
    Timestamp, ///< GPU clock ticks; DeviceLimits::TimestampPeriodNs converts them
    Occlusion, ///< samples that passed the depth test
};

struct QueryPoolDesc
{
    QueryType Type = QueryType::Timestamp;
    u32 Count = 0;
    const char* DebugName = nullptr;
};

// ── Command lists and queues ────────────────────────────────────────────────

/// What a queue does. A device without a dedicated compute or transfer queue runs that work on graphics.
enum class QueueType : u8
{
    Graphics,
    Compute,
    Transfer,
};

enum class CommandListLevel : u8
{
    Primary,   ///< submitted on its own (RHIDevice::Submit)
    Secondary, ///< executed from a primary (RHICommandBuffer::ExecuteSecondary)
};

/// A command list to record into, from RHIDevice::BeginCommandList.
struct CommandListDesc
{
    QueueType Queue = QueueType::Graphics;
    CommandListLevel Level = CommandListLevel::Primary;
    /// The recording thread: each thread records from its own pool (Jobsystem's thread index).
    u32 ThreadIndex = 0;
    /// Secondary lists drawn inside a rendering scope: the attachment formats of that scope.
    bool InsideRendering = false;
    std::array<TextureFormat, 8> ColorFormats{};
    u32 ColorFormatCount = 0;
    TextureFormat DepthFormat = TextureFormat::Undefined;
    const char* DebugName = nullptr;
};

/// Primary lists to run on a queue.
struct SubmitDesc
{
    QueueType Queue = QueueType::Graphics;
    std::span<RHICommandBuffer* const> Lists;
    /// Block until the work finished, then recycle the lists. Off: they are
    /// recycled once the frame slot they were submitted in comes round again.
    bool Wait = true;
};

// ── Descriptors beyond the bindless set ─────────────────────────────────────

/**
 * Set 0 is the engine's bindless set (every texture and buffer by index),
 * which every pipeline has and the convenience BindPipeline binds. A pipeline
 * may add sets 1..3 of these, for resources bound per draw or dispatch.
 */
enum class DescriptorType : u8
{
    UniformBuffer,
    StorageBuffer,
    SampledTexture, ///< combined with the texture's own sampler
    StorageTexture,
};

struct DescriptorBinding
{
    u32 Binding = 0;
    DescriptorType Type = DescriptorType::StorageBuffer;
    u32 Count = 1;
    ShaderStage Stages = ShaderStage::Compute; ///< AllGraphics, Vertex, Fragment or Compute
};

struct DescriptorSetLayoutDesc
{
    std::vector<DescriptorBinding> Bindings;
    /// Written at record time with RHICommandBuffer::PushDescriptors instead of
    /// allocated (needs DeviceLimits::PushDescriptors).
    bool PushDescriptor = false;
    const char* DebugName = nullptr;
};

/// One resource for one binding of a set.
struct DescriptorWrite
{
    u32 Binding = 0;
    u32 ArrayElement = 0;
    DescriptorType Type = DescriptorType::StorageBuffer;
    BufferHandle Buffer = nullptr;
    u64 Offset = 0;
    u64 Range = kWholeSize;
    TextureHandle Texture = nullptr;
};

// ── Textures from elsewhere, and views ──────────────────────────────────────

/// A texture the RHI does not own (an XR swapchain image, an interop surface).
struct TextureImportDesc
{
    void* NativeImage = nullptr; ///< VkImage
    void* NativeView = nullptr;  ///< VkImageView; null: the RHI makes one
    TextureDesc Desc;
};

/// Part of a texture seen as a texture: some mips or layers, or another compatible format.
struct TextureViewDesc
{
    u32 BaseMip = 0;
    u32 MipCount = kRemaining;
    u32 BaseLayer = 0;
    u32 LayerCount = kRemaining;
    TextureFormat Format = TextureFormat::Undefined; ///< Undefined: the texture's own
};

// ── The device ──────────────────────────────────────────────────────────────

/// What a native handle request names (RHIDevice::GetNativeHandle).
enum class NativeObject : u8
{
    Instance,       ///< VkInstance
    PhysicalDevice, ///< VkPhysicalDevice
    Device,         ///< VkDevice
    GraphicsQueue,  ///< VkQueue
    ComputeQueue,   ///< VkQueue (the graphics one without a dedicated family)
    TransferQueue,  ///< VkQueue (likewise)
    Allocator,      ///< VmaAllocator
};

enum class NativeTextureObject : u8
{
    Image,   ///< VkImage
    View,    ///< VkImageView
    Sampler, ///< VkSampler
};

/// What the device can do, read once at startup (RHIDevice::GetLimits).
struct DeviceLimits
{
    std::string DeviceName;
    u32 VendorID = 0;
    u32 DeviceID = 0;
    u32 ApiVersion = 0; ///< VK_MAKE_API_VERSION packing

    u32 MaxTexture2DSize = 0;
    u32 MaxTextureArrayLayers = 0;
    u32 MaxColorAttachments = 0;
    u32 MaxPushConstantsSize = 0;
    u32 MaxBoundDescriptorSets = 0;
    u32 MaxSamples = 1; ///< largest sample count colour and depth both support
    std::array<u32, 3> MaxComputeWorkGroupCount{};
    std::array<u32, 3> MaxComputeWorkGroupSize{};
    u32 MaxComputeWorkGroupInvocations = 0;
    u64 MaxStorageBufferRange = 0;
    u64 MinUniformBufferOffsetAlignment = 0;
    u64 MinStorageBufferOffsetAlignment = 0;

    bool Timestamps = false;
    float TimestampPeriodNs = 0.0f; ///< nanoseconds per timestamp tick

    bool Wireframe = false;
    bool TextureCompressionBC = false;
    bool TextureCompressionASTC = false;
    bool TextureCompressionETC2 = false;
    bool TimelineSemaphores = false;
    bool Synchronization2 = false;
    bool DynamicRendering = false;
    bool PushDescriptors = false;
    bool DedicatedComputeQueue = false;
    bool DedicatedTransferQueue = false;
};

} // namespace UHE::RHI
