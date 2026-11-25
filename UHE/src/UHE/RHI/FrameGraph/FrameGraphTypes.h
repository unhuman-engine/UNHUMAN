#pragma once

#include <array>
#include <functional>
#include <string>
#include <variant>
#include <vector>
#include "UHE/RHI/RHITypes.h"

/**
 * @file FrameGraphTypes.h
 * @brief Vocabulary of the FrameGraph frontend (docs/architecture/framegraph-and-rendergraph.md).
 *
 * The frontend describes a frame by meaning: which passes exist, which logical
 * resources they create, read and write, and for what use. It never sees an
 * image layout, a barrier or a Vulkan type. FrameGraph::Lower() turns the
 * declarations into an FGLoweredFrame, which a backend (the Vulkan render
 * graph) realizes: ordering, culling, barriers and memory stay there.
 */
namespace UHE::RHI
{

namespace FG
{
constexpr u32 kInvalidIndex = 0xFFFFFFFFu;
constexpr u32 kMaxRenderTargets = 8u;
} // namespace FG

/// @brief Logical texture: what a pass asks for, not an allocation.
struct FGTexture
{
    struct Desc
    {
        u32 Width = 0;
        u32 Height = 0;
        TextureFormat Format = TextureFormat::RGBA8_UNORM;
        u32 MipLevels = 1;
        u32 Layers = 1;
        u32 SampleCount = 1;
    };
    using Physical = TextureHandle;
};

/// @brief Logical buffer.
struct FGBuffer
{
    struct Desc
    {
        u64 Size = 0;
        BufferUsageFlags Usage = BufferUsageFlags::None;
    };
    using Physical = BufferHandle;
};

/**
 * @brief A version of a logical resource, valid for the frame that declared it.
 *
 * Every write produces a new version and returns its handle, so the data flow
 * is explicit: a pass that reads `hdr` after `hdr = b.Write(hdr, ...)` reads
 * what that write produced. A handle kept past FrameGraph::Reset() is stale
 * and refused, instead of silently naming whatever reused its index.
 */
template <class T> struct FGHandle
{
    u32 Index = FG::kInvalidIndex;
    u32 Version = 0;
    u32 Frame = 0;

    [[nodiscard]] bool IsValid() const { return Index != FG::kInvalidIndex; }
    [[nodiscard]] bool operator==(const FGHandle& other) const
    {
        return Index == other.Index && Version == other.Version && Frame == other.Frame;
    }
    [[nodiscard]] bool operator!=(const FGHandle& other) const { return !(*this == other); }
};

/// @brief Which queue and kind of work a pass records; decides which usages it may declare.
enum class FGPassType : u8
{
    Graphics = 0,
    Compute,
    Transfer,
};

/// @brief Why a pass reads a resource.
enum class FGRead : u8
{
    Sampled, ///< shader read: texture sampling, uniform/storage buffer read (graphics, compute)
    CopySrc, ///< source of a copy (transfer)
};

/// @brief Why a pass writes a resource.
enum class FGWrite : u8
{
    RenderTarget, ///< colour attachment (graphics, colour formats)
    DepthTarget,  ///< depth attachment (graphics, depth formats, at most one per pass)
    Storage,      ///< storage image or buffer write (compute)
    CopyDst,      ///< destination of a copy (transfer)
};

/// @brief State an imported texture is in when the frame starts, or must be left in when it ends.
enum class FGState : u8
{
    Undefined = 0, ///< contents need not survive (start) / no requirement (end)
    RenderTarget,
    DepthTarget,
    ShaderRead,
    Present,
    Storage,
    CopySrc,
    CopyDst,
};

/// @brief Load/store behaviour of a RenderTarget or DepthTarget write.
struct FGAttachment
{
    LoadOp Load = LoadOp::Clear;
    StoreOp Store = StoreOp::Store;
    std::array<float, 4> ClearColor{0.0f, 0.0f, 0.0f, 1.0f};
    float ClearDepth = 1.0f;
    u32 ClearStencil = 0;
};

/**
 * @brief How code after the frame uses a texture the frame created (FrameGraph::Export).
 *
 * The editor's viewport samples its scene colour in the UI, and picking
 * copies one entity ID out of the entity target: both happen outside the
 * graph, after its passes ran. An export says which state they need it in.
 */
struct FGExport
{
    FGState FinalState = FGState::ShaderRead; ///< the state the frame leaves it in
    bool CopySource = false; ///< also copied from outside the graph (RHIDevice::ReadPixel, ReadPixelAsync)
};

/// @brief A named per-frame value that selects pass variants and is part of the graph hash.
using FGParameter = std::variant<bool, i64, double, std::string>;

enum class FGErrorType : u8
{
    InvalidHandle,   ///< default-constructed, from another frame, or out of range
    StaleVersion,    ///< reads or writes a version that a later write replaced
    ReadBeforeWrite, ///< reads a created resource nothing has written yet
    UsageNotAllowed, ///< usage does not fit the pass type or the format
    DoubleWrite,     ///< one pass writes a resource twice
    ReadAndWrite,    ///< one pass reads and writes the same resource
    TooManyTargets,  ///< more than one depth target or kMaxRenderTargets colour targets
    EmptyDesc,       ///< zero-sized texture or buffer
    DuplicateName,   ///< two resources share a name
    NotExportable,   ///< exports an import, exports twice, or asks for an unusable final state
};

struct FGError
{
    FGErrorType Type = FGErrorType::InvalidHandle;
    std::string Pass;     ///< empty for graph-level errors
    std::string Resource; ///< empty when not about one resource
    std::string Message;
};

[[nodiscard]] UHE_API const char* ToString(FGErrorType type);
[[nodiscard]] UHE_API bool IsDepthFormat(TextureFormat format);

// ─── Lowered frame: what a backend consumes ────────────────────────────────

struct FGLoweredResource
{
    std::string Name;
    bool IsTexture = true;
    FGTexture::Desc Texture;
    FGBuffer::Desc Buffer;
    bool Imported = false;
    bool Backbuffer = false; ///< the image this frame presents; the backend supplies it
    bool Exported = false;   ///< created here and kept past the frame (FrameGraph::Export)
    bool ExportCopySource = false; ///< exported and copied from outside the graph
    TextureHandle ImportedTexture = nullptr;
    BufferHandle ImportedBuffer = nullptr;
    FGState InitialState = FGState::Undefined; ///< imported textures only
    FGState FinalState = FGState::Undefined;   ///< imported and exported textures only
};

/// @brief One declared use, in declaration order within its pass.
struct FGLoweredAccess
{
    u32 Resource = FG::kInvalidIndex; ///< index into FGLoweredFrame::Resources
    u32 Version = 0;                  ///< version read, or version a write produces
    bool IsWrite = false;
    FGRead Read = FGRead::Sampled;
    FGWrite Write = FGWrite::RenderTarget;
    FGAttachment Attachment; ///< RenderTarget / DepthTarget writes only
};

struct FGLoweredPass
{
    std::string Name;
    FGPassType Type = FGPassType::Graphics;
    u32 Index = 0; ///< declaration index: FrameGraph::ExecutePass(Index, ...)
    std::vector<FGLoweredAccess> Accesses;
};

struct FGLoweredFrame
{
    std::vector<FGLoweredResource> Resources;
    std::vector<FGLoweredPass> Passes; ///< declaration order
    u64 Hash = 0;                      ///< structure of the frame: equal hash, equal graph
};

} // namespace UHE::RHI
