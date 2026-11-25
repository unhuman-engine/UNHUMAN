# FrameGraph frontend

> **Status:** Phases 1 to 6 DONE (2026-11-25); the deferred items remain. Issue
> [#4](https://github.com/unhuman-engine/UNHUMAN/issues/4).
> Design: [framegraph-and-rendergraph.md](../architecture/framegraph-and-rendergraph.md).

## Why

The render graph runs every frame and does the hard part: ordering, culling,
lifetimes and barriers. But engine and game code reached it through
`dynamic_cast<VulkanDevice*>`, recorded with a raw `vk::CommandBuffer`, and had to
create, size, register and destroy every attachment by hand. The FrameGraph is the
layer above it: passes declared by usage, with no Vulkan types, realized by the
render graph. It does not track dependencies itself, which was the reason the
first version of this layer was dropped in September.

```cpp
struct SceneData { FGHandle<FGTexture> Color, Depth; };
graph->AddPass<SceneData>("Scene",
    [&](FGBuilder& b, SceneData& d) {
        d.Color = b.Write(graph->ImportBackbuffer(device.GetBackbufferDesc()), FGWrite::RenderTarget);
        d.Depth = b.Write(b.Create("Depth", {w, h, TextureFormat::D24_UNORM_S8}), FGWrite::DepthTarget);
    },
    [](const SceneData&, FGContext& ctx) { /* ctx.Cmd() is an RHICommandBuffer& */ });
```

## Layers

| Layer | Where | Owns |
|---|---|---|
| FrameGraph | `UHE/src/UHE/RHI/FrameGraph/` | typed, versioned handles; usages; blackboard; parameters; validation; hash; lowering |
| Bridge | `UHE/src/Platform/Vulkan/RenderGraph/VulkanFrameGraphBridge.*` | lowered frame to render graph declarations; pooled textures; the pass context |
| Render graph | `UHE/src/Platform/Vulkan/RenderGraph/` | order, culling, lifetimes, image states, barriers, execution |

Usage maps to the render graph without new backend vocabulary: `RenderTarget` is a
write plus a colour attachment, `DepthTarget` a write plus a depth attachment,
`Storage` a compute write, `CopyDst` a transfer write, `Sampled` a read,
`CopySrc` a transfer read. A usage that does not fit the pass type or the format is
a frontend error.

## Phases

| Phase | Deliverable | Status | Tested by |
|---|---|---|---|
| 1 | Frontend: `FGHandle<T>` (index, version, frame), `Create`/`Import`/`ImportBackbuffer`, `AddPass<Data>(setup, execute)`, usages, blackboard, parameters, `Lower()`, `Hash()` | `DONE` | `tests/framegraph` |
| 2 | Bridge: translation, `TransientTexturePool` (reuse by shape, retire after 3 unused frames), `FGContext` with `RHICommandBuffer&`, `RHIDevice::GetFrameGraph()`/`GetBackbufferDesc()` | `DONE` | `tests/framegraph` (pool), Sandbox golden image |
| 3 | Graph errors stop the run under `UHE_VALIDATION_FATAL` | `DONE` | broken backbuffer size exits 1 with the gate, 0 without |
| 4 | Passes move to the FrameGraph: Sandbox, Editor and AimLab (no `dynamic_cast`, no Vulkan types left in app code); the Sandbox renders Scene (RGBA16F) then `PostProcess::AddTonemapPass` (ACES) into the backbuffer | `DONE` | golden image, editor validation run |
| 5 | `VulkanRenderGraphCompileCache` reuses the compiled frame when the declared frame is structurally identical, compared by exact key, with handles rebound to the current generation; `UHE_RENDERGRAPH_NO_CACHE=1` turns it off | `DONE` | `tests/rendergraph` hit/miss/staleness; cached and uncached frames byte-identical |
| 6 | Graph-created buffers from a `TransientBufferPool` (one `TransientPool` template for both kinds) | `DONE` | `tests/framegraph` (pool); no engine pass creates a buffer yet |
| later | Per-pass command buffers and parallel recording (needs per-thread command pools), memory aliasing, compute-queue hints, an editor panel listing passes, culled passes and barriers | `DEFERRED` | — |

## Decisions

- **Frostbite-style declaration** (2026-11-25): pass data in a struct filled by
  setup, read by execute, shareable through the blackboard.
- **The frontend is flushed into the render graph before the UI pass**, so its
  passes record first and the UI draws on top. The device resets it at the end of
  every frame, skipped and failed ones included; handles from a past frame are
  refused.
- **The backbuffer is imported by the device**, so the UI pass and a frame graph
  pass that render into it share one render graph slot.

## Found and fixed on the way

- **No barrier between two scopes rendering one attachment.** The compiler
  skipped a barrier when layout and guard were unchanged, so the UI pass's load
  was not ordered after the scene's writes. Now every write is followed by one.
- **Attachment access left out the read.** `ColorWrite`/`DepthWrite` mapped to
  write bits only; a load, a blend and a depth test read the attachment.
- **Graph errors were silent.** A failing pass was dropped and the run exited 0.
- **Four RHI formats had no Vulkan mapping** (R8, RG8, RGBA16F, RGBA32F); the
  first HDR target reached `vkCreateImage` as UNDEFINED, caught by the gate.
- **The topology hash is not a cache key.** It counts a pass's reads instead of
  naming them; the compile cache compares an exact key instead.

## Verified

Moving the Sandbox scene onto one FrameGraph pass changed no pixel: 0 of 1,755,027
differ locally, and CI's lavapipe golden image passed unchanged. The HDR and
tonemap frame is the new golden image. Cached and uncached compiles render the
same frame byte for byte. Sandbox and editor run clean under
`UHE_VALIDATION_FATAL=1`.

UHEGAME (AimLab) cannot be run on any branch: `UHEGAME/assets` has no shaders or
fonts on `master`. Its pass compiles and uses the editor's pattern, which runs.
