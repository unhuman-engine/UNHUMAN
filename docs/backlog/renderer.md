# Renderer Backlog (Vulkan)

> **Status:** mirror of [#7](https://github.com/unhuman-engine/UNHUMAN/issues/7), items split into
> [#25](https://github.com/unhuman-engine/UNHUMAN/issues/25) and [#29](https://github.com/unhuman-engine/UNHUMAN/issues/29).
> **Index:** [../README.md](../README.md) · **Roadmap:** [../ROADMAP.md](../ROADMAP.md)
> Last truth-up: 2026-09-13 against `improve_vulkan` @ `c271467`.

This file used to live at `UHE/src/Platform/Vulkan/rendererTODO.md` and drifted out of date
(items were ticked in code but not here). It is now a **status mirror only** — the work order
lives in [../ROADMAP.md](../ROADMAP.md), the detailed designs live in
[../architecture/vulkan-sync-and-rendergraph.md](../architecture/vulkan-sync-and-rendergraph.md)
and [../architecture/framegraph-and-rendergraph.md](../architecture/framegraph-and-rendergraph.md).

Status vocabulary: `DONE` · `IN PROGRESS` · `STUB` (files exist, no behaviour) · `TODO` · `DEFERRED`.

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
- [ ] RenderGraph builder/compiler/executor — **STUB** — `Platform/Vulkan/RenderGraph/*.{h,cpp}` all
      exist but have no bodies (`VulkanRenderGraphBuilder`, `Compiler`, `Executor`, `Resources`,
      `Types`); unreferenced. Feature code will declare passes here directly, see
      [../ROADMAP.md](../ROADMAP.md) M5
