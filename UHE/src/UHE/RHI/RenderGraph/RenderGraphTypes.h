#pragma once
#include <functional>
#include <string>
#include <vector>
#include "UHE/RHI/RHIBarrier.h"
#include "UHE/RHI/RHITypes.h"

// Render graph step 1 types (docs/architecture/rendergraph.md §7, §14 step 1).
//
// This layer declares and validates the frame; it never touches the GPU. Every
// handle is index+generation; every write versions a resource so the future
// compiler (§8.3) can turn read/write order into graph edges and barriers.
//
// Sync decisions implemented here (docs/architecture/rendergraph.md §9):
//   - §9.1.2 phase 1: RGQueue is carried but nothing splits on it yet.
//   - §9.1.4: imported resource descs are part of the topology hash, so
//     RGImportedDesc hashes to the same material set as RGTextureDesc.

namespace UHE::RHI
{

namespace RG
{
constexpr u32 kInvalidIndex = 0xFFFFFFFFu;
constexpr u32 kStartGeneration = 1u;
constexpr u32 kMaxColorAttachments = 8u;
} // namespace RG

// ─── Handles ────────────────────────────────────────────────────────────────
// Index into the registry slot array; generation catches stale handles after
// reuse. Valid means index+generation match a live registry entry.

struct TextureTag
{
};
struct BufferTag
{
};

template <class Tag> struct RGHandle
{
    u32 index = RG::kInvalidIndex;
    u32 generation = 0;

    [[nodiscard]] bool IsValid() const { return index != RG::kInvalidIndex && generation != 0; }
    [[nodiscard]] bool operator==(const RGHandle& other) const
    {
        return index == other.index && generation == other.generation;
    }
    [[nodiscard]] bool operator!=(const RGHandle& other) const { return !(*this == other); }
};

using RGTextureHandle = RGHandle<TextureTag>;
using RGBufferHandle = RGHandle<BufferTag>;

template <class Tag> struct RGHandleHasher
{
    [[nodiscard]] size_t operator()(const RGHandle<Tag>& handle) const
    {
        return (static_cast<size_t>(handle.index) << 32) ^ handle.generation;
    }
};

// ─── Resource descriptions ──────────────────────────────────────────────────
// Hash-comparable: two descs with equal material content produce equal hashes
// (the topology cache adds names/extent/tier on top, §9 hash-input list).
// floats are bit-hashed, never value-compared, so 0.0f == -0.0f is harmless.

struct RGTextureDesc
{
    std::string name; // debug only — excluded from the hash
    TextureFormat format = TextureFormat::RGBA8_UNORM;
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;
    u32 layers = 1;
    u32 sampleCount = 1;
    TextureUsage usage = TextureUsage::ColorAttach;
    ImageState initialState = ImageState::Undefined;
    ImageState finalState = ImageState::Undefined; // e.g. Present for swapchain

    [[nodiscard]] u64 Hash() const;
    [[nodiscard]] bool MateriallyEquals(const RGTextureDesc& other) const;
};

struct RGBufferDesc
{
    std::string name; // debug only — excluded from the hash
    u64 size = 0;
    BufferUsageFlags usage = BufferUsageFlags::None;
    bool hostVisible = false;

    [[nodiscard]] u64 Hash() const;
    [[nodiscard]] bool MateriallyEquals(const RGBufferDesc& other) const;
};

// Declares how an external texture enters the graph. The imported texture keeps
// its initial state; the graph's finalState for it lives in the texture desc.
struct RGImportedDesc
{
    u64 hashPayload = 0; // e.g. swapchain generation or buffer device address
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 sampleCount = 1;

    [[nodiscard]] u64 Hash() const;
    [[nodiscard]] bool MateriallyEquals(const RGImportedDesc& other) const;
};

// ─── Pass declarations ──────────────────────────────────────────────────────

enum class RGPassType : u8
{
    Graphics = 0,
    Compute,
    Transfer,
    RayTracing // reserved (rendergraph.md §10.6)
};

// Phase 1 (§9.1.2): carried on passes and compiled output, nothing splits on it
// until a second queue family exists (rendergraph.md §13.1 item 1).
enum class RGQueue : u8
{
    Graphics = 0,
    Compute,
    Transfer
};

struct RGColorAttachment
{
    RGTextureHandle texture{};
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    RGTextureHandle resolve{}; // MSAA resolve target
};

struct RGDepthAttachment
{
    RGTextureHandle texture{};
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    float clearDepth = 1.0f;
    u32 clearStencil = 0;
};

// One declared read or write of a resource, bound to the version that was
// current at declaration time: a write carries the new version it produces, a
// read the latest version that existed (rendergraph.md §7 versioning rules).
// The compiler turns these (resource, version) pairs into edges and barriers.
struct RGResourceAccess
{
    RGTextureHandle texture{};
    RGBufferHandle buffer{};
    bool isTexture = true;
    u32 version = 0; // texture or buffer version this access is bound to
};

// Defined by the executor milestone (rendergraph.md §8.5). Step 1 only stores
// execute callbacks; the parameter type is incomplete until then, which is
// fine — completeness is only required when the callback is invoked.
class RGPassContext;
using RGPassExecuteFn = std::function<void(RGPassContext&)>;

struct RGPassSpec
{
    std::string name;
    RGPassType type = RGPassType::Graphics;
    RGQueue queue = RGQueue::Graphics;
    std::vector<RGColorAttachment> colors;
    RGDepthAttachment depth{};
    bool hasDepth = false;
    std::vector<RGResourceAccess> reads;
    std::vector<RGResourceAccess> writes;
    RGPassExecuteFn execute; // stored in step 1; invoked by the executor (§8.5)
    /// The pass's recording touches only its own state and the resources it
    /// declared, so it may record on another thread at the same time as any
    /// other pass (per-pass command lists, issue #32). Off: it records after
    /// the pass before it, as renderers with global state need.
    bool parallelRecording = false;
};

// ─── Validation results ─────────────────────────────────────────────────────
// Compile collects all errors before failing (rendergraph.md §8.3 step 1).

enum class RGErrorType : u8
{
    StaleHandle = 0,
    UndeclaredAttachment, // attachment texture not declared with Write()
    DoubleWrite,          // same resource written twice by one pass
    ReadBeforeWrite,
    UnsubmittedPass, // AddPass() without Execute()/Submit() — nameless placeholder
    // Compiler-stage validation (rendergraph.md §8.3 stages 1–2):
    PassStateConflict, // one pass needs one resource in two different layouts
    PassCycle,         // Kahn's queue emptied early — dependency cycle
};

struct RGValidationError
{
    RGErrorType type;
    std::string passName; // empty for graph-level errors
    std::string resourceName;
    std::string message;
};

// ─── Raw-graph dump (§14 step 1 deliverable; compiler consumes later) ───────

struct RGDeclaredTexture
{
    std::string name;
    u32 versionCount = 0;
    bool imported = false;
    ImageState initialState = ImageState::Undefined;
    ImageState finalState = ImageState::Undefined;
};

struct RGDeclaredBuffer
{
    std::string name;
    u32 versionCount = 0;
    bool imported = false;
};

struct RGDeclaredPass
{
    std::string name;
    RGPassType type = RGPassType::Graphics;
    RGQueue queue = RGQueue::Graphics;
    u32 readCount = 0;
    u32 writeCount = 0;
    u32 colorCount = 0;
    bool hasDepth = false;
    std::vector<std::string> readNames;  // "Name.vN"
    std::vector<std::string> writeNames; // "Name.vN"
};

struct RGRawGraph
{
    u64 topologyHash = 0;
    std::vector<RGDeclaredTexture> textures;
    std::vector<RGDeclaredBuffer> buffers;
    std::vector<RGDeclaredPass> passes; // declaration order
    std::vector<RGValidationError> errors;
    [[nodiscard]] bool HasErrors() const { return !errors.empty(); }
};

// ─── Logging helpers ────────────────────────────────────────────────────────
// Local to the graph .cpps; names are deliberately namespaced to avoid
// colliding with future RHI-wide ToString helpers.

[[nodiscard]] const char* RGToString(TextureFormat format);
[[nodiscard]] const char* RGToString(ImageState state);
[[nodiscard]] const char* RGToString(RGPassType type);
[[nodiscard]] const char* RGToString(RGQueue queue);
[[nodiscard]] const char* RGToString(LoadOp op);
[[nodiscard]] const char* RGToString(StoreOp op);

} // namespace UHE::RHI
