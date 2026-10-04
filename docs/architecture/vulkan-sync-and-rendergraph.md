<!-- UHE docs — index: ../README.md · roadmap: ../ROADMAP.md -->

> **Status:** `DESIGN`, not implemented. Related issues:
> [#2](https://github.com/unhuman-engine/UNHUMAN/issues/2),
> [PR #27](https://github.com/unhuman-engine/UNHUMAN/pull/27),
> [#4](https://github.com/unhuman-engine/UNHUMAN/issues/4),
> [#14](https://github.com/unhuman-engine/UNHUMAN/issues/14).
> **Code lands in:** `UHE/src/Platform/Vulkan/`.
> **Snapshot:** written against `improve_vulkan` at `c271467`; `file:line`
> references are snapshots and should be re-verified before acting.

# Vulkan sync and render graph

This document reviews the current synchronization and render-graph state and
proposes the type and ownership model for a synchronization layer and a render
graph that the job system can drive. Scope is `UHE/src/Platform/Vulkan/` and
`UHE/src/UHE/Jobsystem/`.

## Summary

1. Sync2 is requested but never used. `VulkanExtensionCheck` can enable
   `VK_KHR_synchronization2`, but every barrier is still the legacy
   `vkCmdPipelineBarrier` with `PipelineStageFlags`, and `VulkanDevice::End()`
   still calls `vkQueueSubmit`. There is a capability flag but no fallback
   policy.
2. Capability detection is extension-only. It ignores core promotion (Sync2 at
   1.3, timeline semaphores at 1.2, dynamic rendering at 1.3), so capable
   devices can be misclassified. Timeline semaphores are always off because of a
   one-line bug, `IsEnable("")`.
3. The render graph is an empty stub. The types and ownership model should be
   settled before writing it, and it should build on `TaskGraph`.
4. Proposed layering: `SyncPolicy`, `BarrierEncoder`, `SubmitEncoder`, and a
   render graph made of a compiler, a resource pool and an executor that emits
   `TaskGraph` nodes and records per-thread command buffers.

## Current state

| Area | Location | Status |
|---|---|---|
| Extension and feature detection | `VulkanExtensionCheck.cpp` | Extension-name only, no core-version promotion |
| Feature chain build | `VulkanExtensionCheck.cpp:140` | Correct chaining, gated on the detection above |
| Timeline semaphore | `VulkanSemaphore.cpp:11` | Always binary because of `IsEnable("")` |
| Binary semaphore and fence wrappers | `VulkanBinarySemaphore.*`, `VulkanBinaryFence.h` | Stubs; `VulkanBinaryFence.h` redefines the wrong class |
| Barriers | `VulkanCommandBuffer.cpp:67,268` | Legacy v1 barriers with hardcoded layouts |
| Submit and present | `VulkanDevice.cpp:331` | Legacy `vkQueueSubmit`, no `vkQueueSubmit2` path |
| Frame-in-flight sync | `VulkanFrameContext.*` | Per-frame `imageAvailable` plus fence, per-image `renderFinished` |
| Immediate submit | `VulkanDevice.cpp:378` | One pool and fence, whole-pool reset, not thread-safe |
| Render graph | `RenderGraph/RenderGraphVulkan.*` | Empty stub |
| Render job wrapper | `RenderGraph/JobSystem.*` | Empty stub, superseded by the engine job system |
| JobSystem and TaskGraph | `UHE/Jobsystem/*` | Implemented, tested and documented |

### Extension detection ignores core promotion

`GetEnabledDeviceExtensions()` and `TickTheAvailableExtension()` only inspect
`vkEnumerateDeviceExtensionProperties`. On a Vulkan 1.3 driver the Sync2 and
dynamic-rendering extensions are often not advertised because they are core, so
`HasVkSync2` stays false, the feature is never requested, and `IsEnable()`
returns false. The application then silently takes the fallback path on modern
hardware.

Capability should be separated from the extension string:

```
Sync2Supported    = apiVersion >= 1.3 || ext(VK_KHR_synchronization2)
TimelineSupported = apiVersion >= 1.2 || ext(VK_KHR_timeline_semaphore)
DynamicRendering  = apiVersion >= 1.3 || ext(VK_KHR_dynamic_rendering)

EnableExtension(name) = only if not core-promoted at the device apiVersion
EnableFeature(struct) = only after vkGetPhysicalDeviceFeatures2 confirms it
```

Features are requested through the version structs
(`VkPhysicalDeviceVulkan12Features`, `VkPhysicalDeviceVulkan13Features`) when
core, or the KHR structs when using the extension, never both for the same bit,
since the specification forbids duplicate feature structs in the `pNext` chain.
The code already chains the version structs unconditionally, which is the right
base. The extension structs in `BuildDeviceFeatureChain()` should be skipped for
promoted features.

### Known defects

These are independent of the larger design and are tracked by the sync issues.

1. `VulkanSemaphore.cpp:11` uses `IsEnable("")`, so `m_IsTimeline` is always
   false and `WaitCPU` and `GetValue` are no-ops. It should use a core-aware
   timeline check.
2. `VulkanBinaryFence.h:12` declares `class VulkanBinarySemaphore`, a second
   definition of the class from `VulkanBinarySemaphore.h` with different member
   access. Including both in one translation unit is an ODR violation. Rename it
   to `VulkanBinaryFence`.
3. `VulkanLogicalDevice.h:40-43` has an unused `requiredDeviceExtension` member.
   Either remove it or wire it in.
4. `VulkanCommandBuffer.cpp:100,149,181` hardcodes barrier `oldLayout` to
   `Undefined`. It must come from tracked resource state; `Undefined` is only
   valid the first time an image is used.
5. `VulkanCommandBuffer.cpp:222,229` creates a `VulkanFramebuffer` as a local on
   every `BeginRenderPass` on the legacy path and destroys it at scope end while
   the render pass still references it. This is a lifetime bug.
6. `VulkanDevice::ImmediateSubmit` resets the whole upload pool, which is safe
   only while single-threaded. A concurrent upload corrupts it.
7. `VulkanDevice::ReadPixel` allocates and frees a readback buffer per call and
   blocks on a full submission. It should be pooled and deferred.

## Synchronization architecture

### Goals

- One call site, two backends. Renderer code requests a wait at given stages; the
  policy emits either `vkCmdPipelineBarrier2` and `vkQueueSubmit2` or the legacy
  APIs.
- Correct on Android and Vulkan 1.1, with no timeline semaphores, Sync2 or
  dynamic rendering required.
- Thread-safe recording through per-job-thread command pools and deferred
  destruction keyed on GPU completion.
- Timeline-first frame pacing on capable hardware, with a fence fallback.

### Capability tiers

```cpp
enum class SyncTier {
    Legacy,       // Vulkan 1.1: vkCmdPipelineBarrier, vkQueueSubmit, fences
    Sync2,        // 1.3 or extension: vkCmdPipelineBarrier2, vkQueueSubmit2
    Sync2Timeline // Sync2 plus timeline semaphores
};
SyncTier tier = SelectTier(physicalDevice); // apiVersion + features + extensions
```

The tier is detected once at device init and stored next to `VulkanContext`.
Higher layers branch on the tier, never on extension strings at the call site.

### Barrier types

```cpp
enum class Stage : u64 { None, Top, Transfer, Compute, Vertex, Fragment,
                         ColorOutput, DepthEarly, DepthLate, Bottom };
enum class Access : u64 { None, TransferRead, TransferWrite, ShaderRead,
                          ShaderWrite, ColorWrite, DepthWrite, MemoryRead, MemoryWrite };
enum class ImageState { Undefined, ColorAttachment, DepthAttachment,
                        ShaderRead, TransferSrc, TransferDst, Present, Storage };

struct ImageBarrier  { RGTextureHandle img; ImageState old, next; Stage srcStage, dstStage; Access src, dst; };
struct BufferBarrier { RGBufferHandle  buf; Stage srcStage, dstStage; Access src, dst; };
struct GlobalBarrier { Stage srcStage, dstStage; Access src, dst; };
```

`BarrierEncoder` translates these. On the legacy tier it maps each stage and
access to the nearest v1 bit, coalesces per image into `VkImageMemoryBarrier`
and emits one `vkCmdPipelineBarrier`. On the Sync2 tier it emits
`vkCmdPipelineBarrier2` with `VkDependencyInfo` and `VkImageMemoryBarrier2`,
where the 64-bit stages make the mapping lossless.

`ImageState` is the single source of truth for layouts, replacing the hardcoded
`oldLayout = Undefined` and the ad-hoc `TransitionLayout` calls. The render graph
computes transitions at compile time; manual passes call the encoder directly.

### Semaphore model

```cpp
class VulkanSemaphore {
    enum class Type { Binary, Timeline };
    void Signal(u64 value);       // GPU side, via pSignalSemaphoreValues
    void Wait(u64 value, Stage);  // GPU side wait
    u64  GetValue();              // CPU, timeline only, otherwise 0
    void WaitCPU(u64 value);      // timeline: semaphore wait; legacy: fence wait
};
```

On the Sync2Timeline tier a single frame timeline semaphore advances once per
submitted frame, and the CPU waits on the frame index before reusing that frame's
resources, replacing the wait-on-fence-then-reset pattern. On the legacy tier the
frame timeline becomes a per-frame fence, `WaitCPU` waits on the fence, and
`GetValue` returns a CPU-maintained counter. GPU-to-GPU dependencies use binary
semaphores in a chain. Any API that requires a timeline value is emulated with a
binary pair and a fence; the render-graph features that degrade should be
documented.

### Submit model

```cpp
struct SubmitInfo {
    std::span<const SemaphoreWait> waits;     // {sem, stage}
    std::span<const CommandBufferRef> cmds;
    std::span<const SemaphoreSignal> signals; // {sem, value}
};
class SubmitEncoder {
    void Submit(vk::raii::Queue&, const SubmitInfo&);
};
```

The Sync2 tier uses `vkQueueSubmit2` with `VkSubmitInfo2` and
`VkSemaphoreSubmitInfo`. The legacy tier uses `vkQueueSubmit` with
`pWaitDstStageMask`. Present stays a separate call: acquire, record, submit
signalling `renderFinished[image]`, then present waiting on
`renderFinished[image]`.

### Frame loop

```
BeginFrame(frame):
    if timeline: wait frameTimeline >= frame->lastSubmitted
    else:        wait and reset frame->fence
    acquireNextImage(imageAvailable, &imageIndex)
    reset per-frame command pools
    deletionQueue[frame].FlushSemaphores()

EndFrame():
    submit(cmd, wait=imageAvailable,
           signal=renderFinished[image],
           signal=frameTimeline=++counter or frame->fence)
    present(renderFinished[image])
    frame = (frame + 1) % N
```

Relative to the current `VulkanDevice::Begin` and `End`: `renderFinished` is
already per image and should stay that way; `imageAvailable` could also become
per image to avoid the acquire-reuse edge case. The swapchain resize path should
wait on the timeline counter rather than `waitIdle()`, so resizing does not stall
the CPU.

### Deferred destruction

`DeletionQueue` currently flushes per frame index. With a timeline, each queued
item is tagged with the frame value at destruction and flushed once that value
has passed. On the legacy tier the per-frame queues are kept and flushed only
after the frame fence is confirmed. This also unblocks the job system, since
workers can `DeferDestruction()` safely if the queue is per-thread or mutexed.

### Multithreaded command recording

The job system provides stable thread indices via `GetCurrentThreadIndex()`. The
proposal is a per-thread command pool and command buffer. Render passes record
into secondary command buffers on worker threads, and the frame's primary buffer
calls `vkCmdExecuteCommands`. Pools use `eTransient | eOneTimeSubmit` for
per-pass secondary buffers and `eResetCommandBuffer` for the frame primary.
Submission is serialized on the main thread, as Vulkan requires. This is the
bridge to the render-graph executor.

## Render graph architecture

### Layers

```
user code -> RenderGraphBuilder
             -> RenderGraphCompiler   (cull, order, alias, barriers, queues)
             -> RenderGraphExecutor   (TaskGraph, BarrierEncoder, SubmitEncoder)
             -> ResourcePool (VMA)    (transient textures and buffers, aliasing)
```

A `RenderGraph` facade owns one of each. It stays in
`Platform/Vulkan/RenderGraph/`; the concept is backend-neutral, but the only
backend is Vulkan, so the compiler may emit `BarrierEncoder` calls.

### Core types

```cpp
// Typed handle, index plus generation, no lifetime.
template <class Tag> struct RGHandle { u32 index; u32 generation; };

struct RGTextureDesc {
    std::string name;
    TextureFormat format;
    u32 width, height, mipLevels = 1, layers = 1;
    TextureUsage usage;               // reuse RHI::TextureUsage
    bool transient = true;            // pool-owned vs imported
    bool allowAliasing = true;
    u32 imported = kInvalid;
};

struct RGBufferDesc { std::string name; u64 size; BufferUsage usage; bool transient = true; };

enum class RGPassType { Graphics, Compute, Transfer, RayTracing };

struct RGColorAttachment { RGHandle<RGTextureTag> tex; LoadOp load; StoreOp store; ClearValue clear; };
struct RGDepthAttachment  { RGHandle<RGTextureTag> tex; LoadOp load; StoreOp store; float clearDepth; u8 clearStencil; };

struct RGRenderTargetInfo {
    RGColorAttachment colors[8]; u32 colorCount = 0;
    RGDepthAttachment depth; bool hasDepth = false;
};

using RGPassExecute = std::function<void(RGPassContext&)>;
```

`RGPassContext` exposes only the current command buffer, the resolved images,
image views and buffers for declared attachments, push-constant and descriptor
helpers, and viewport and scissor. It does not expose barrier APIs, because
barriers are the compiler's responsibility.

### Setup API

```cpp
RenderGraph rg;
RGTextureHandle swap  = rg.Import("Swapchain", swapchainTexture);
RGTextureHandle depth = rg.Import("Depth", depthTexture);

RGTextureDesc gbd { .name="GBufferAlbedo", .format=RGBA8_UNORM, .width=w, .height=h,
                    .usage=TextureUsage::ColorAttach|TextureUsage::Sampled };

auto& pass = rg.AddGraphicsPass("GBuffer");
auto albedo = pass.Write(gbd);
pass.Write(depth);
pass.SetExecute([=](RGPassContext& ctx){
    ctx.BindPipeline(gbufferPipeline);
    ctx.DrawIndexed(...);
});

auto& lit = rg.AddGraphicsPass("Lighting");
lit.Read(albedo);
lit.Write(swap);
lit.SetExecute(...);

rg.Compile();
rg.Execute();
```

Resources are versioned. `pass.Write(desc)` creates a new version and
`pass.Read(handle)` binds to the latest version at build time. This turns
read-after-write, write-after-read and write-after-write hazards into explicit
graph edges and removes hand-written barriers.

### Compile phase

1. Topological sort by resource-version edges, with cycle detection.
2. Dead-pass culling. A pass whose written resources are never read, and which
   does not write an imported or presented resource, is removed. Lighting output
   to the swapchain is a root.
3. Lifetime analysis per resource, `[firstWrite, lastRead]` in pass order.
4. Memory aliasing. Transient resources with disjoint lifetimes share a VMA
   allocation, bucketed by size, format and memory type, with greedy interval
   packing. Report aliasing waste in debug builds.
5. Queue assignment. Each pass declares a queue, and the compiler inserts
   cross-queue edges and acquire and release barriers.
6. Barrier synthesis. For each resource version, compute the transition from old
   state to new state and the stage and access masks from the consuming pass
   type, then hand the list to `BarrierEncoder`. On the legacy tier, coalesce
   adjacent barriers between passes.
7. Command-buffer strategy. Decide how many secondary buffers each pass uses and
   where merges go.

The compiled schedule is cached on a hash of the pass list, resource
descriptions and extent, reused across frames while the topology is stable, and
rebuilt on resize or feature toggle.

### Resource pool and aliasing

```
ResourcePool
  TextureRegistry : RGTextureHandle -> VkImage, VkImageView, current ImageState
  BufferRegistry  : RGHandle        -> VulkanBuffer
  AliasedHeap     : transient allocations bucketed by memory type, VMA-backed
```

Imported resources bypass the pool. Transient resources are allocated at compile
time from the alias heap and released when the frame's timeline value passes,
never touched on the hot path. `ImageState` is tracked here, and the compiler
reads and writes it to compute barriers.

### Execution and job-system integration

Each compiled pass becomes a `TaskNode`:

```
TaskID id = graph.CreateTask(&RecordPass, &passContext);
for (each dependency edge p -> pass) graph.AddDependency(id_p, id);
```

`RecordPass` calls the pass execute function into a secondary command buffer from
`perThread[GetCurrentThreadIndex()]`. The primary frame buffer is built on the
main thread and executes the secondary buffers in compiled or queue order. Passes
on different queues become separate `TaskGraph`s submitted at the cross-queue
synchronization points derived during compile. The root task owns primary
Begin and End, acquire, per-queue submit, present and timeline signalling.

```
acquire -> main/frame -> ExecuteCommands -> submit(graphics, compute, transfer) -> present
              |
     Shadow, GBuffer, Lighting  (TaskGraph nodes, parallel record)
```

Constraints to encode: Vulkan submission is single-threaded, so only the root
task submits; secondary buffers can only record within a render-pass-compatible
scope, and dynamic rendering in a secondary buffer is fine on 1.3; on the legacy
tier dynamic rendering is off, so the compiler groups passes by `VkRenderPass`
and `VkFramebuffer` and caches those objects.

### Fallback behaviour

| Feature | Sync2Timeline | Sync2 | Legacy (1.1, Android) |
|---|---|---|---|
| Barriers | barrier2 | barrier2 | barrier, coalesced |
| Submit | QueueSubmit2 | QueueSubmit2 | QueueSubmit |
| Frame pacing | timeline value | fences | fences |
| Graphics passes | dynamic rendering | dynamic rendering | cached render passes |
| Memory aliasing | yes | yes | yes |
| Cross-queue | timeline and binary | binary | binary and fences |

The render-graph API is identical across tiers; only the two encoders differ.

### Debug and validation

Name every resource and pass via `VK_EXT_debug_utils`, and add a debug scope
marker per pass for RenderDoc. Barrier logging can dump each transition per frame
and assert that no `Undefined` to `Undefined` no-ops survive. At compile time,
reading a never-written imported resource is a hard error that names the pass.

## Proposed file layout

```
Platform/Vulkan/
  Sync/
    VulkanSyncTier.h/.cpp
    VulkanBarrierEncoder.h/.cpp
    VulkanSubmitEncoder.h/.cpp
    VulkanSemaphore.h/.cpp
    VulkanBinarySemaphore.*
    VulkanFence.* / VulkanBinaryFence.*
  RenderGraph/
    RenderGraph.h/.cpp
    RenderGraphTypes.h
    RenderGraphBuilder.h/.cpp
    RenderGraphCompiler.h/.cpp
    RenderGraphResources.h/.cpp
    RenderGraphExecutorVulkan.h/.cpp
```

`RenderGraph/JobSystem.*` should be deleted once the executor uses
`UHE/Jobsystem/Taskgraph.h`.

## Order of work

1. Fix the ground truth first, meaning the detection, timeline and layout
   defects above. Nothing else is testable until the tier is reported
   accurately.
2. Add `VulkanBarrierEncoder` and `VulkanSubmitEncoder` and route
   `VulkanCommandBuffer::BeginRenderPass` and `EndRenderPass` and
   `VulkanDevice::Begin` and `End` through them. Behaviour on modern GPUs should
   be unchanged, now via v2 APIs. Add a `--force-legacy` toggle.
3. Add a legacy-tier CI run on Lavapipe with Sync2 disabled. This is the Android
   safety net.
4. Track resource state in the resource pool, replacing hardcoded `Undefined`,
   still without the full graph.
5. Add the render-graph types, builder and compiler for a single queue with
   dynamic rendering only, executing through the existing `TaskGraph`.
6. Add memory aliasing and cross-queue edges, plus the render-pass pipeline state
   cache and multithreaded command recording.
7. Wire multithreaded secondary recording to `GetCurrentThreadIndex()`.

## will deside soon

- One frame timeline or per-queue timelines. Per-queue timelines give finer
  cross-queue ordering but complicate the legacy fallback. Start with one frame
  timeline and binary semaphores for cross-queue edges.
- How aggressively to split a pass into secondary buffers. Measure before
  defaulting to more than a few.
- The graph caching key. A hash of pass and resource descriptors is enough, but
  dynamic per-frame resources such as shadows and clusters need stable slot
  indices so the cache survives.
- RenderGraph scope. Keep it inside the Vulkan backend for now and promote it
  only if a second backend becomes real.
- Descriptor management. The executor should bind the global set once and expose
  per-pass sets through `RGPassContext` rather than letting passes reach into
  `VulkanDescriptorManager`.
