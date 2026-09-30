# Project Idea — UNHUMAN Engine (UHE)

> Original note, kept verbatim: *"it old project very old year old but i working actively in it"*
>
> Newest design docs and the work order live in [docs/](docs/) — start at
> [docs/README.md](docs/README.md), then [docs/ROADMAP.md](docs/ROADMAP.md).

## What UHE is

A hand-written, cross-platform C++ game engine: Vulkan RHI (no abstraction framework doing the
thinking for me), Slang shaders with runtime SPIR-V generation, entt ECS, Jolt/Box2D physics,
Jolt in progress, ImGui editor, Tracy profiling, a JobSystem + TaskGraph, and a sandbox "game" layer
used as the only honest acceptance test.

It is a long-running project — started years ago, rewritten more than once, actively developed now.

## Why it exists

1. **To understand the graphics stack from the bottom.** The point of writing a Vulkan backend by
   hand is that the failure modes of a real driver become legible: synchronization, descriptor
   indexing, layout transitions, capability gating. That knowledge is why RADV/ACO work is possible
   at all, and it feeds back into it.
2. **To be an honest artifact.** Not a tutorial clone: a tree with architecture docs, a roadmap, an
   issue tracker, CI with sanitizers, and features that can be demonstrated in a scene. Anything
   claimed in the readme should be reproducible from a checkout.
3. **To support the graphics work that funds the hardware.** The engine is where new driver-facing
   techniques get tried before they are interesting to Mesa.

## Who it is for

- Primarily: me, as an R&D vehicle and portfolio artifact.
- Secondarily: anyone who wants to read a real Vulkan backend rather than a tutorial. That means
  docs that describe the *actual* state and distinguish "implemented" from "planned".

## Hard constraints (these are not negotiable scope)

- **Low-end floor is Vega 7 / GFX9 (Vulkan 1.1–1.2).** Any design that silently requires 1.3+ is
  wrong. Capability tiers exist for this reason.
- **Runs on Linux (primary) and Windows.** Android is a target for the *fallback path*'s sake, not a
  product.
- **Solo developer.** Every architecture decision is judged by "can one person finish this and keep
  it working", which is why the docs reject big-bang rewrites and phase everything.
- **No fabricated results.** No benchmark numbers or feature claims that CI or a scene can't show.

## What it deliberately is not (yet)

- Not a shipping game engine for third parties: no marketplace, no scripting sandbox, no editor
  polish budget.
- Not multi-backend: the RHI is thin so a second backend is *possible*, not so it gets written now.
- Not a renderer feature checklist for its own sake. Features land when a sandbox scene needs them.

## Current state (2026-09-13)

| Area | State |
|---|---|
| Vulkan backend | Works on GFX9; descriptor backend modernised; sync2 path plus a 1.1 fallback in progress (PR #27) |
| JobSystem + TaskGraph | Implemented and documented |
| Rendering | Deferred-ish single-pass renderer, PBR-ish shading, MSDF text |
| glTF | Node hierarchy, shared-mesh instancing, non-indexed geometry and meshoptimizer pass all land (#29 tier 0 + most of tier 1). Still open: `COLOR_0` vertex colour, `KHR_texture_transform`, sampler wrap/filter on load, mipmaps on the model path, `alphaMode: BLEND` ordering |
| Audio | miniaudio integration landed; assets/voices/buses not designed in yet |
| Assets | Path-based, no registry, `AssestsManager` naming debt |
| Render graph | Compiles, resolves and executes every frame in `VulkanDevice::EndFrameGraph()`; carries the ImGui pass only. Scene rendering still records via immediate submit — plan at `docs/plans/PLAN-render-graph-scene-pass.md` |
| CI | Builds + headless Lavapipe smoke + sanitizer workflows; no in-tree unit tests |
| Editor | ImGui editor, docking, gizmos, drag-drop; entity hierarchy is single-parent only (#17) |

## Direction of travel

Sync/multi-pass correctness first (tiers → graph), then material and renderer correctness against a
scene, then assets/audio on the job system, then tooling breadth. The order and the reasoning are in
[docs/ROADMAP.md](docs/ROADMAP.md).
