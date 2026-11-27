#pragma once
#include <string>
#include <vector>
#include "UHE/RHI/RenderGraph/RenderGraphResources.h"
#include "UHE/RHI/RenderGraph/RenderGraphTypes.h"

// Render graph compiler (docs/architecture/rendergraph.md §8.3) — step 2 slice.
//
// Stages implemented (§14 step 2: "topo/cull/lifetime/barriers"):
//   1. Validate   — builder rules already ran (facade); this adds per-pass use
//                   conflicts (one resource in two different states in a pass)
//                   and pass-cycle detection.
//   2. Topo sort  — Kahn's algorithm over version edges (producer of vN →
//                   readers of vN). With declaration-time version binding the
//                   declaration order is already topological; Kahn's verifies
//                   and exists for the later queue-batching stages.
//   3. Cull       — live = backward-reachable from roots (any pass touching an
//                   imported resource). Culled passes are reported by name.
//   4. Lifetime   — [firstUse, lastUse] (+ firstWrite) per resource, in
//                   compiled order. Aliasing (§8.2) consumes this later.
//   6. State track— per-texture layout walk; transitions derived from the
//                   use matrix below, never routed through Undefined between
//                   two defined states (the oldLayout hardcode fix).
//   7. Barriers   — grouped per pass boundary: image layout transitions, buffer
//                   barriers, dedup per resource. Global barriers stay optional
//                   in the pass struct; v1 always emits per-resource barriers.
//   9. Emit       — RGCompiledFrame, POD-style data so re-execution needs no
//                   re-locking (the Blender lesson).
//
// Step-2 inert parts (§8.3 stages 5 and 8): aliasing (needs the §8.2 pool) and
// queue batching (single queue family today, §9.1.2 phase 1) are NOT here —
// RGQueue is carried on CompiledPass for the executor to place.
//
// Output is pure data with engine enums only. The executor resolves RG handles
// to vk::Image/vk::Buffer and fills VulkanBarrierEncoder's structs — the
// compiler never sees Vulkan types.
//
// Deliberate deltas over the §8.4 sketch:
//   - CompiledPass carries `passIndex` (declaration index) instead of
//     `RGPassExecuteFn*` — a pointer into the builder's pass vector would
//     dangle across frame reset/redeclaration.
//   - CompiledPass gains `postImageBarriers`: the desc's finalState contract
//     (e.g. ColorAttachment → Present on the swapchain) needs an exit
//     transition, which the §8.4 sketch has no home for.
//   - Graphics-pass buffer reads map to Stage::Vertex (uniform/vertex input);
//     buffer writes belong in compute/transfer passes in v1.

namespace UHE::RHI
{

// One image layout transition at a pass boundary. Old/Next are engine
// ImageStates; the executor turns this into VulkanBarrierEncoder::ImageBarrier.
struct RGCompiledImageBarrier
{
    RGTextureHandle texture{};
    ImageState oldState = ImageState::Undefined;
    ImageState newState = ImageState::Undefined;
    Stage srcStage = Stage::None;
    Stage dstStage = Stage::None;
    Access srcAccess = Access::None;
    Access dstAccess = Access::None;
    /// A queue-family ownership transfer when the two differ: the release half
    /// on the source queue, the acquire half on the destination (async compute).
    RGQueue srcQueue = RGQueue::Graphics;
    RGQueue dstQueue = RGQueue::Graphics;
};

struct RGCompiledBufferBarrier
{
    RGBufferHandle buffer{};
    Stage srcStage = Stage::None;
    Stage dstStage = Stage::None;
    Access srcAccess = Access::None;
    Access dstAccess = Access::None;
    RGQueue srcQueue = RGQueue::Graphics; ///< see RGCompiledImageBarrier
    RGQueue dstQueue = RGQueue::Graphics;
};

/// A graphics pass that uses what an async compute pass made: the graphics
/// submission waits for the compute one, at \p waitStage.
struct RGCrossQueueEdge
{
    u32 producer = 0; ///< compiled id, on the compute queue
    u32 consumer = 0; ///< compiled id, on the graphics queue
    Stage waitStage = Stage::AllCommands;
};

/// How the compiler may place passes.
struct RGCompileOptions
{
    /// The device has a dedicated compute queue: passes declared with
    /// RGQueue::Compute may run on it (Stage 8). Off: every pass is graphics.
    bool asyncCompute = false;
};

struct RGCompiledPass
{
    u32 id = 0;        // compiled order (execution order)
    u32 passIndex = 0; // declaration index — resolves the spec + execute fn
    std::string name;
    RGPassType type = RGPassType::Graphics;
    RGQueue queue = RGQueue::Graphics;
    std::vector<RGCompiledImageBarrier> preImageBarriers;
    std::vector<RGCompiledImageBarrier> postImageBarriers; // finalState exits, queue releases
    std::vector<RGCompiledBufferBarrier> preBufferBarriers;
    std::vector<RGCompiledBufferBarrier> postBufferBarriers; // queue releases
    std::vector<u32> dependencies;                           // compiled indices of passes that precede
};

// [firstUse, lastUse] per resource in compiled order; aliasing (§8.2) and the
// VRAM report consume this (§13.3 item 15).
struct RGCompiledResourceLifetime
{
    bool isTexture = true;
    RGTextureHandle texture{};
    RGBufferHandle buffer{};
    std::string name;
    bool imported = false;
    u32 versionCount = 0;
    u32 firstUsePass = 0;
    u32 lastUsePass = 0;
    u32 firstWritePass = RG::kInvalidIndex; // never written = imported-only
};

struct RGCompiledFrame
{
    u64 topologyHash = 0;               // §8.3 cache key (caching itself lands in §13.3 item 14)
    u32 declaredPassCount = 0;          // pre-cull, debug
    std::vector<RGCompiledPass> passes; // topological, live only
    std::vector<RGCompiledResourceLifetime> lifetimes;
    std::vector<std::string> culledPasses; // debug UX: which passes were cut
    std::vector<RGCrossQueueEdge> crossQueueEdges;
    /// Passes that asked for async compute and run on graphics, with why.
    std::vector<std::string> demotedPasses;
    [[nodiscard]] bool UsesAsyncCompute() const { return !crossQueueEdges.empty() || HasComputeQueuePass(); }
    [[nodiscard]] bool HasComputeQueuePass() const
    {
        for (const RGCompiledPass& pass : passes)
            if (pass.queue == RGQueue::Compute)
                return true;
        return false;
    }
};

struct RGCompileResult
{
    RGCompiledFrame frame;
    std::vector<RGValidationError> errors;
    [[nodiscard]] bool Ok() const { return errors.empty(); }
};

class RenderGraphCompiler
{
public:
    RenderGraphCompiler() = default;
    RenderGraphCompiler(const RenderGraphCompiler&) = delete;
    RenderGraphCompiler& operator=(const RenderGraphCompiler&) = delete;
    ~RenderGraphCompiler() = default;

    // Compiles the declared passes into an executable frame. Collects all
    // errors (never throws); a failed result carries an empty frame.
    // `passes` must be the builder's declaration-ordered list; `resources`
    // supplies slot metadata (imported initial states, version logs, names).
    [[nodiscard]] static RGCompileResult Compile(const std::vector<RGPassSpec>& passes,
                                                 const RenderGraphResources& resources,
                                                 const RGCompileOptions& options = {});
};

} // namespace UHE::RHI
