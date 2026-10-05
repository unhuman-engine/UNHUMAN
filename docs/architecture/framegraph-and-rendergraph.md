<!-- UHE docs — index: ../README.md · roadmap: ../ROADMAP.md -->

> **Status:** `DESIGN`. The target shape of the RHI and graph layers, reached in
> five phases. Related issues:
> [#4](https://github.com/unhuman-engine/UNHUMAN/issues/4),
> [#14](https://github.com/unhuman-engine/UNHUMAN/issues/14),
> [#12](https://github.com/unhuman-engine/UNHUMAN/issues/12).
> **Code lands in:** `UHE/src/UHE/RHI/` for the frontend and
> `UHE/src/Platform/Vulkan/RenderGraph/` for the backend, currently a stub.
> **Snapshot:** written against `improve_vulkan` at `c271467`; `file:line`
> references should be re-verified before acting.

# FrameGraph, RenderGraph and RHI layering

Companion to [vulkan-sync-and-rendergraph.md](vulkan-sync-and-rendergraph.md).
This document covers the ownership split: semantic frame description in the
frontend, physical realization in the backend, on an RHI that exposes enough
control to make that possible.

## The split

```
FRAME GRAPH (frontend)
  "this frame needs shadow, gbuffer, lighting, present"
  semantic, logical resources, no Vulkan, no barriers
        | logical resource IDs and access intent
RENDER GRAPH (backend)
  physical resources, lifetimes, aliasing, image states, barriers,
  queue assignment, submit ordering, command recording
        | resolved handles and explicit commands
RHI CORE (thin, explicit, near 1:1)
        |
      Vulkan
```

| Layer | Knows about | Owns | Does not own |
|---|---|---|---|
| FrameGraph | render features and logical passes | pass graph, culling intent, blackboard, parameters | GPU handles, layouts, barriers, memory |
| RenderGraph | physical resources, state, queues | lifetimes, aliasing, transitions, barriers, submit plan | feature semantics |
| RHI core | device objects, command encoding | resources, barriers as commands, queues, sync primitives | pass scheduling, lifetimes |
| RHI convenience | ergonomics | `BeginRenderPass` sugar, model submission, default samplers | hidden synchronization |

This is the FrameGraph and render-graph split used in Frostbite and UE. The
frontend changes when a feature changes; the backend changes when the hardware or
API changes.

## Why the current RHI blocks a render graph

| Current API | Problem |
|---|---|
| `RHICommandBuffer::BeginRenderPass(RenderPassDesc)` inserts barriers (`VulkanCommandBuffer.cpp:67-266`) | Two owners of synchronization. The graph cannot choose layouts or stages because the encoder does it silently. |
| `EndRenderPass` hardcodes transitions to `ShaderReadOnly` (`:303-345`) | Layouts depend on the next consumer, which only the graph knows. |
| Handles are `void*` pointer encodings (`RHITypes.h:23-27`) | No generation or versioning, so resource versions cannot be distinguished. |
| `TextureDesc` is minimal (`RHITypes.h:154`) | No sample count, layers, view type, memory flags, initial state or import path. |
| `RHICommandBuffer` has no barrier, copy, blit, dispatch, secondary or timestamp commands | The graph cannot express what it schedules. |
| Only one graphics queue (`VulkanDevice.h:58`) | No transfer or compute overlap, no cross-queue edges. |
| Global descriptor set bound in `BindPipeline` (`:366`) | Per-pass descriptor flexibility is hidden. |
| `RenderPassDesc` is a value struct with fixed array sizes | Couples the graph's compile step to one render-pass encoding and blocks dynamic rendering and render-pass caching. |

A render graph can only be as capable as the command interface beneath it. If the
RHI hides state, the graph cannot compute it.

## Two-tier RHI

Keep the ergonomic API, but stop making it the only API.

```
RHI Convenience (thick)   BeginRenderPass, SubmitModel, ...   used by existing code
      implemented on
RHI Core (thin)           Barrier, BeginRendering, Dispatch,  used by RenderGraph
                          Copy, queues, semaphores, full descs
      implemented on
Vulkan
```

RenderGraph depends only on RHI Core. Convenience is reimplemented on Core so
existing call sites such as the editor and game layers keep working. The hidden
barrier logic moves out of `BeginRenderPass` into the render graph's pass encoder;
the convenience `BeginRenderPass` remains only as a standalone helper for code
outside the graph.

### What exposing the core means

This is not about exposing raw `Vk*` everywhere. It means engine enums that map
one to one to Vulkan at the core tier:

```cpp
struct TextureDesc {
    // existing fields
    u32 sampleCount = 1;
    u32 arrayLayers = 1;
    bool cube = false;
    TextureViewType view = TextureViewType::SRV;
    ResourceState initialState = ResourceState::Common;
    MemoryUsage memory = MemoryUsage::Default;
    bool importExternal = false;
};

void PipelineBarrier(std::span<const ImageBarrier>,
                     std::span<const BufferBarrier>,
                     const GlobalBarrier*);
void BeginRendering(const RenderingInfo&);   // no auto-barriers
void EndRendering();
void CopyBufferToImage(...); void CopyImage(...); void BlitImage(...);
void Dispatch(u32 x, u32 y, u32 z);
void DispatchIndirect(...);
void BeginDebugLabel(const char*); void EndDebugLabel();
void WriteTimestamp(QueryHandle, Stage);

RHICommandBuffer* AllocateSecondary(CommandPoolHandle);
void Submit(QueueType, const SubmitDesc&);
```

Barrier and state enums are the same ones defined in the sync document, so the
barrier encoder is shared.

### Coverage and policy

A useful way to define "thin" is two independent axes rather than one.

```
                little policy        lots of policy
   broad        thin RHI             convenience layer
   coverage     all capabilities     BeginRenderPass, SubmitModel
                no decisions

   narrow       raw subset           current RHI: few capabilities,
   coverage                          and the ones present hide state
```

Coverage is how much of the hardware can be expressed: copy, blit, dispatch,
barriers, secondary buffers, queues, queries, full resource descriptions, views
and import. Policy is how much the layer decides without asking: implicit
barriers, hardcoded layouts, default memory, auto-bound descriptors, fixed
render-pass arrays.

A production thin RHI has broad coverage and low policy. It is thin in
intelligence, not in capability. The current RHI is narrow coverage with heavy
hidden policy, which is why it cannot host a render graph. The fix is to broaden
coverage and remove policy; those are separate tasks and both are required.

### Principles

1. No hidden synchronization. Barriers are commands and state is tracked by the
   caller or by an explicit tracker.
2. Resource creation mirrors the hardware: mips, layers, samples, cubemaps, view
   type, memory usage, import.
3. Near one-to-one recording: barrier, begin rendering, draw, dispatch, copy,
   blit, resolve, query, secondary.
4. Explicit execution: queues are values, submits take a `SubmitDesc`, and sync
   primitives are first-class.
5. Typed generational handles plus a native escape hatch.
6. No engine concepts such as `Model`, `Camera` or `EditorCamera`.
7. Capability queries instead of capability assumptions, so upper layers adapt
   without `#ifdef`.
8. Caller-owned or explicitly deferred lifetime, with no GC guesswork.
9. Debug metadata from the start: labels, names, timestamps.
10. Branching confined to platform files. The interface stays backend-neutral and
    Sync2 or legacy and Vulkan or DX12 live behind it.

A thin RHI does not schedule passes, compute lifetimes, alias memory, choose
layouts or pick queues. That belongs to the graph layers.

### How thin to be

Full cross-backend abstraction is not needed yet. The interface is still worth
having, because the render graph needs a stable seam and the graph should be
testable without a GPU.

- Keep the interface backend-neutral in types and Vulkan-shaped in semantics.
- Prefer non-virtual inline wrappers over a backend function table on the
  recording path. Virtual interfaces at command granularity are also fine; the
  dispatch cost is small next to submission.
- Make the escape hatch first-class. `GetNativeHandle()` returns the underlying
  object so extensions such as descriptor buffer or mesh shaders do not force the
  core interface to grow early.
- Add a second backend only when there is a real target.

### Gaps to close

| Missing | Needed by |
|---|---|
| Barriers and image-state tracking | render graph compiler |
| Compute dispatch | lighting, post, culling |
| Copy, blit, resolve, buffer-to-image copy | uploads, mip generation, MSAA |
| Secondary command buffers and pools | multithreaded recording |
| Queue types and `SubmitDesc` | transfer and compute overlap |
| Descriptor set layout, set and push descriptors | per-pass data |
| Real pipeline interface | flexible pipelines |
| Queries and timestamps | profiling, occlusion |
| Full `TextureDesc` | graph resources, MSAA, shadow arrays |
| Native-handle escape hatch | extensions |
| Feature and limits query returned to callers | graph tier selection |
| Explicit render pass and framebuffer | legacy fallback backend |

This is the backlog for making the RHI a real thin layer, and it is also what the
render graph needs, which is a sign the ownership split is in the right place.

## The three mapping boundaries

### FrameGraph to RenderGraph

The frontend hands over logical resource versions, pass read and write access,
pass parameters and a queue hint. It does not hand over handles.

```cpp
FGHandle<Texture> albedo = fg.Create<Texture>("GBufferAlbedo", desc);
auto& gbuffer = fg.AddPass("GBuffer", PassQueue::Graphics);
gbuffer.Write(albedo, WriteUsage::RenderTarget);
gbuffer.SetExecute([](FrameGraphContext& ctx){ /* ... */ });

auto& lighting = fg.AddPass("Lighting", PassQueue::Graphics);
lighting.Read(albedo, ReadUsage::SRV);
```

### RenderGraph to RHI Core

The compiler has already decided the physical resource, state transition, queue,
barriers and command-buffer strategy. The executor calls only the core.

```cpp
for (RGCompiledPass& pass : compiled) {
    RHICommandBuffer* cmd = pass.isPrimary ? &frameCmd
                                           : pool.AllocateSecondary(threadIndex);
    cmd->Begin();
    encoder.EmitBarriers(*cmd, pass.preBarriers);
    if (pass.type == PassType::Graphics) {
        cmd->BeginRendering(pass.renderingInfo);
        pass.record(*cmd, ctx);
        cmd->EndRendering();
    }
    cmd->End();
}
```

### RHI Core to Vulkan

Mechanical and small. This is where Sync2 or legacy branching lives, along with
resource creation and queue handling.

## FrameGraph frontend

```cpp
template <class Tag> struct FGHandle { u32 index; u32 version; };

enum class ReadUsage  { SRV, IndirectArgs, CopySrc, DepthRead, Present };
enum class WriteUsage { RenderTarget, DepthWrite, UAV, CopyDst, Clear };

class FrameGraph {
    template<class T> FGHandle<T> Create(std::string_view name, const ResourceDesc&);
    template<class T> FGHandle<T> Import(std::string_view name, T* existing);

    FrameGraphPassBuilder& AddPass(std::string_view name, PassQueue hint);

    void Compile();
    void Execute(RenderGraph& backend);
    void Clear();
};

class FrameGraphContext {
    template<class T> T* Get(FGHandle<T>);
    Blackboard& Board();
};
```

Frontend responsibilities:

- Culling: a pass whose outputs are never consumed, and which is not presented or
  written to an imported resource, is dropped.
- Blackboard: typed per-frame data such as the camera, the light list and frame
  statistics, so passes do not reach through globals.
- Parameters: `fg.SetParameter("CascadeCount", 4)` feeds pass variants and the
  cache key.
- Async safety: passes reference handles, never pointers into stack frames.

## RenderGraph backend

```cpp
struct RGPhysicalTexture {
    TextureHandle rhi;
    ResourceState currentState;
    Range<u32> lifetime;      // [firstWrite, lastRead] in pass order
    bool imported = false;
    u32 aliasSlot = 0;
};

struct RGCompiledPass {
    PassType type;
    QueueType queue;
    std::vector<ImageBarrier> preBarriers;
    std::vector<BufferBarrier> preBarriersBuf;
    RenderingInfo renderingInfo;
    RecordFn record;
    std::vector<u32> dependencies;   // TaskGraph edges
};

class RenderGraphCompiler {
    CompiledFrame Compile(FrameGraphIR&, const CompileOptions&);
};

class RenderGraphExecutor {
    void Execute(CompiledFrame&, UheJobsystem&, RHI&);
};
```

The compiled frame is cacheable on a hash of pass names, resource descriptions,
parameters and extent, so the schedule is reused while the topology is stable.

## Swapchain and present

Ownership has to be explicit, otherwise acquire and present sit outside the graph
and layout ownership becomes ambiguous:

- The swapchain image is an imported texture with initial state `Present`.
- The final pass writes it and requests final state `Present`.
- The executor's root task acquires before the graph and presents after, and emits
  the `ColorAttachment` to `Present` barrier from the compiled output.
- Resize invalidates the imported handle and forces a graph rebuild.

## Migration phases

Each phase should be independently shippable.

Phase 0, freeze semantics: this document and the sync document. Decide enum
names, the handle scheme and the rule that RenderGraph depends only on RHI Core.

Phase 1, split RHI into core and convenience: no behavior change. Move the
hidden barriers out of `BeginRenderPass` into a standalone helper, and introduce
core methods the helper calls. Existing code is untouched.

Phase 2, expose core flexibility: barriers, dynamic-rendering begin and end
without auto-barriers, copy, blit, dispatch, secondary buffers, queries, full
resource descriptions and `SubmitDesc`, verified by running the current renderer
through the core.

Phase 3, render-graph backend, one pass at a time: shadow, then GBuffer, then
lighting. Passes that are not migrated run as a single legacy node in the same
`TaskGraph`, so execution and submission stay unified.

Phase 4, framegraph frontend: once at least two passes are in the backend, add
the frontend, move feature code to declare logical resources, and add culling and
the blackboard. The frontend `Compile()` calls the backend compiler.

Phase 5, backend intelligence: aliasing, cross-queue and async compute, graph
caching, and validation or debug overlays.

| Today | Becomes |
|---|---|
| `Scene::OnUpdate` calling `Renderer3D::SubmitModel` | framegraph pass writing a logical color target |
| Hidden barriers in `VulkanCommandBuffer::BeginRenderPass` | render-graph compiler barrier synthesis |
| `RenderPassDesc` in `RHITypes.h` | `RenderingInfo` resolved by the render graph |
| `void*` handles | typed generational handles at core, `FGHandle<T>` at the frontend |
| Global descriptor set in `BindPipeline` | per-pass descriptor view through `FrameGraphContext` |
| `VulkanDevice::Begin` and `End` | executor root task: acquire, submit, present |

## Difficulty

The genuinely hard parts:

1. Handing synchronization ownership to the graph. Until then there are two
   schedulers, which is the main source of subtle bugs.
2. The handle and versioning scheme. Logical versions are cheap, but names and
   generations pay off when debugging.
3. Resource aliasing. Correct once lifetimes are stable, not before.
4. Multi-queue and async compute. Deferred.
5. The legacy fallback. Dynamic rendering versus render-pass caching multiplies
   the compiler's work, so fallback should be a compiler option rather than a
   second graph.
6. The descriptor model. Bindless-as-global conflicts with per-pass flexibility,
   so support both.

The parts that are mostly mechanical: the RHI Core to Vulkan boundary, the
compiler's sort, cull and lifetime passes, TaskGraph integration at one node per
pass, and extending the enums that already exist in `RHITypes.h` and
`VulkanExtensionCheck.h`.

## Decisions before coding

- Where FrameGraph lives. Proposed: `UHE/RHI/FrameGraph/`, since it must not
  include Vulkan. The render graph backend stays in
  `Platform/Vulkan/RenderGraph/`.
- One graph or one graph per view. Handles are view-agnostic now; per-view
  subgraphs can come later for split-screen or VR.
- When the graph is rebuilt. Rebuild on pipeline, resource or parameter change,
  and cache otherwise, with the hash inputs defined explicitly.
- Async compute ambition. If wanted, passes should declare queue hints from the
  start even if the compiler ignores them in phase 3.
- Import versus create. Anything owned outside, such as the swapchain, external
  render targets and model textures, is imported; everything else is transient.

## Starting point

The enabling work is a thin RHI core plus moving barriers into the graph; after
that the FrameGraph and RenderGraph mapping is mostly types and glue. This is a
change of ownership rather than a renderer rewrite. The existing Vulkan objects,
pipelines, descriptors and swapchain stay, gain a lower-level entry point, and
gain a scheduler above them.

The first commit should be phase 1: add the core methods and reimplement the
existing `BeginRenderPass`, `EndRenderPass` and submit on top of them, with no
behavior change.
