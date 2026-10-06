# Renderer Backlog (Vulkan)

> **Status:** mirror of [#7](https://github.com/unhuman-engine/UNHUMAN/issues/7), items split into
> [#25](https://github.com/unhuman-engine/UNHUMAN/issues/25) and [#29](https://github.com/unhuman-engine/UNHUMAN/issues/29).
> **Index:** [../README.md](../README.md) · **Roadmap:** [../ROADMAP.md](../ROADMAP.md)
> Last truth-up: 2026-10-01 against `asset-improvement` @ `785959e`.

This file used to live at `UHE/src/Platform/Vulkan/rendererTODO.md` and drifted out of date
(items were ticked in code but not here). It is now a **status mirror only** — the work order
lives in [../ROADMAP.md](../ROADMAP.md), the detailed designs live in
[../architecture/vulkan-sync-and-rendergraph.md](../architecture/vulkan-sync-and-rendergraph.md)
and [../architecture/framegraph-and-rendergraph.md](../architecture/framegraph-and-rendergraph.md).

Status vocabulary: `DONE` · `IN PROGRESS` · `STUB` (files exist, no behaviour) · `TODO` · `DEFERRED`.


## glTF / GLB loader — mirror of [#29](https://github.com/rajaryan2007/unhuman/issues/29)

Status vocabulary as above. Loader lives in `UHE/src/UHE/Renderer3D/LoadModel.{h,cpp}` plus
`LoadModelGeometry.cpp` (accessors + meshoptimizer), `LoadModelUpload.cpp` (RHI),
`LoadModelAnimation.cpp` (skins/animation). Harness: `tests/gltf` (70 checks, no GPU).

### Tier 0 — structure (all DONE, `785959e`)

- [x] Node hierarchy in `ProcessNode` — accumulated `parentTransform * NodeLocalTransform`, applied at
      submit as `transform * mesh.LocalTransform`. Not baked into vertices, so skinning still composes.
- [x] Shared mesh instancing — one `Geometry` per glTF mesh, one `Mesh` instance per node
- [x] Non-indexed geometry — sequential indices synthesized
- [x] meshoptimizer integrated — cache optimize, fetch remap, vertex merge, tangent generation
- [x] Out-of-range material index clamped at load
- [x] Unsupported extensions reported by name (`extensionsUsed`/`extensionsRequired`)

### Tier 1 — material completeness

- [x] `baseColorFactor`
- [x] `emissiveFactor` + `emissiveTexture` (factor defaults to BLACK per spec, not white)
- [x] `normalTexture` + `scale` — TANGENT attribute added at location 5; generated per-corner and
      seam-split before vertex merge
- [x] `occlusionTexture` + `strength` (defaults to 1.0, not 0.0)
- [x] `alphaMode` + `alphaCutoff` — OPAQUE and MASK supported
- [x] `doubleSided` + pipeline cull mode — `CullMode` added to `RHIDevice`; second pipeline variant
      per material. Backend default stays `eNone` so existing pipelines render unchanged.
- [x] Node transforms in `ProcessNode`
- [x] sRGB — pipeline/swapchain formats are `RGBA8_SRGB`
- [ ] `COLOR_0` vertex colour — **TODO** — needs a 4th vertex attribute + shader input
- [ ] `KHR_texture_transform` — **TODO** — currently *listed as unsupported*; the UV V-flip is
      hardcoded (`LoadModelGeometry.cpp:154`) and conflicts with it. Options: (a) keep flip and drop
      the extension, (b) remove flip and add an asset-level convention flag, (c) implement the
      extension so it owns UV convention. (c) is the only one that leaves no known-wrong path.
- [ ] Texture sampler wrap/filter on load — **TODO** — glTF declares `magFilter`/`minFilter`/`wrapS`/
      `wrapT` per texture; `VulkanUtils.cpp:204 CreateSampler` exists but the loader does not pass them
- [ ] Mipmaps on the model texture path — **TODO** — `VulkanTexture::GenerateMipmaps` exists and is
      called from one path (`VulkanTexture.cpp:209`), but not for textures loaded via the glTF loader.
      Without it, minified foliage aliases badly.
- [ ] `alphaMode: BLEND` — **STUB** — parsed and flagged via `Model::HasTransparentMaterials()`, logs a
      warning that it draws in submission order. Needs a depth-sorted transparent pass, which depends on
      the render-graph scene pass landing first.

### Known non-bugs (investigated, retracted)

- Texture loads appearing "24 times instead of 8" — that was one model load per manual drag-drop in an
  editor test session. Per load, each material loads its own image once. There is no texture dedup in
  the loader (no cache keyed by image index), so assets where several materials share one image pay per
  material — routine in foliage/tiled assets, absent from character models.

## Descriptor / fallback

- [x] Descriptor set binding for fallback — **DONE** (`VulkanDescriptorManager/Pool/Set`, PR #6/#11)
- [ ] Descriptor set path in the *frontend* — **TODO** — [#14](https://github.com/unhuman-engine/UNHUMAN/issues/14); backend supports the new path, `Renderer3D`/editor still call the old one
- [ ] Synchronization of `1.1` for fallback (Android-shaped path) — **TODO** — [#2](https://github.com/unhuman-engine/UNHUMAN/issues/2), design §2.2 capability tiers in the sync doc
- [ ] Compute feature — **IN PROGRESS** — `VulkanComputePipeline.{h,cpp}` builds through
      `RHIDevice::CreateComputePipeline`; `RHICommandBuffer::Dispatch` and compute pipeline creation
      are wired (`37b7660`). Still no compute shader asset, no caller dispatching, and
      `VulkanComputePipeline::ShutDownComputePipeline` is never invoked.
- [ ] Async compute + resource reuse — **TODO** — sync doc §2.5/§2.8, framegraph doc Phase 5

## Pipeline & render pass

- [ ] Pipeline state caches for render passes and fallbacks — **TODO** — sync doc §5 step 6
- [ ] Render graph — see the Render graph section below

## Command submission

- [x] Job system — **DONE** — `UHE/Jobsystem/{Jobsystem,Taskgraph}` landed in `c271467`, documented in [../architecture/jobsystem.md](../architecture/jobsystem.md) (issue [#5](https://github.com/unhuman-engine/UNHUMAN/issues/5) is still open only for the upgrades listed there, §10)
- [ ] Multithreaded command buffer recording and submission — **TODO** — sync doc §2.8; blocked on per-thread command pools in `VulkanDevice::ImmediateSubmit`

## Shader system

- [ ] Shader system with SPIR-V generation via Slang — **PARTIAL** — `SlangCompiler.{h,cpp}` + `Shader.{h,cpp}` compile and load; `Platform/Vulkan/ShaderSystem/VulkanShaderManager.{h,cpp}` is an unreferenced stub
- [ ] Hot reload for shaders — **TODO** — assets doc §2.7

## Render graph

- [ ] FrameGraph frontend as a second layer — **CANCELLED 2026-09-24** — dropped; it duplicated
      dependency tracking and put two graph layers between feature code and the backend. Can be
      layered on later if renderer code ever needs a stable API that outlives backend changes.
- [ ] RenderGraph builder/compiler/executor — **PARTIAL** — the builder/compiler/executor/resources
      all have working bodies and run per frame: `VulkanDevice::EndFrameGraph()` compiles, resolves,
      maps to a TaskGraph and executes (`VulkanDevice.cpp:579`). Covered by `tests/rendergraph`.
      **Carries only the ImGui pass** — the single `AddPass` call site outside `RenderGraph/` is
      `VulkanDevice.cpp:551`. `Renderer3D` still records via immediate submit, so no scene rendering
      flows through the graph yet. Plan: `docs/plans/PLAN-render-graph-scene-pass.md`.
- [ ] Scene pass on the render graph — **TODO** — `Renderer3D::EndScene()` is an empty body and no
      render scope is opened for 3D; depth for the scene depends on ImGui's pass. Moving
      `SubmitModel` into a graph pass makes it deferred (passes execute after `End()` calls
      `EndFrameGraph()`). Immediate submit is kept deliberately as an escape hatch.
- [ ] Frame-graph failure is silent — **TODO** — `m_FrameGraphFailed` latches at
      `VulkanDevice.cpp:584/600/617` and `End()` falls back to a legacy swapchain transition. A
      compile error in a new scene pass would drop the whole scene with only a log line.
