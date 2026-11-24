# FrameGraph frontend

> **Status:** IN PROGRESS. Phases 1 and 2 are done, Phase 3 is done, Phase 4 has
> started (the Sandbox). Issue [#4](https://github.com/unhuman-engine/UNHUMAN/issues/4).
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
| 4 | Passes move to the FrameGraph: Sandbox (done), then Editor and AimLab (their `dynamic_cast` and `RGPassContext` go), then Scene to HDR to Tonemap with a second golden image | `IN PROGRESS` | golden images, editor validation run |
| 5 | Reuse the compiled render graph while `Hash()` is unchanged | `TODO` | hit/miss tests |
| 6 | Graph-created buffers (a buffer pool like the texture pool) | `TODO` | `tests/framegraph` |
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

## Verified

The Sandbox renders its scene as one FrameGraph pass. Its frame is byte-identical to
the one the old direct path produced (0 of 1,755,027 pixels differ), and the
lavapipe golden image is unchanged. Sandbox and editor run clean under
`UHE_VALIDATION_FATAL=1`.
