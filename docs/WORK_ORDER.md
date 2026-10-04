# Work Order

Sequential reference. The detailed map is [ROADMAP.md](ROADMAP.md); this file is
the short "what comes next" list. Current focus: Vulkan sync, then
multithreading.

The order follows two constraints: barriers must have an owner before multi-pass
work is safe, and command recording must be thread-safe before the render graph
records in parallel.

| Order | Stage | Work | Issues |
|---|---|---|---|
| 1 | Vulkan sync (current) | Capability tiers, then barrier and submit encoders, then a legacy fallback toggle, then a CI legacy leg, then timeline semaphores and deferred destruction | [#2](https://github.com/unhuman-engine/UNHUMAN/issues/2), [PR #27](https://github.com/unhuman-engine/UNHUMAN/pull/27), [#31](https://github.com/unhuman-engine/UNHUMAN/issues/31) |
| 2 | Multithreading (current) | Per-thread command pools, work-stealing queues, coroutine continuations, secondary command-buffer recording with single-threaded submit, TaskGraph driving the frame | [#5](https://github.com/unhuman-engine/UNHUMAN/issues/5), [#37](https://github.com/unhuman-engine/UNHUMAN/issues/37) |
| 3 | RHI core and descriptors | Explicit barriers, copy and dispatch, queues and SubmitDesc, descriptor frontend with a global set bound once and per-pass sets | [#30](https://github.com/unhuman-engine/UNHUMAN/issues/30), [#14](https://github.com/unhuman-engine/UNHUMAN/issues/14) |
| 4 | RenderGraph backend | Resource-state tracking, builder and compiler (sort, cull, barriers), executor on TaskGraph, pipeline and render-pass caches | [#4](https://github.com/unhuman-engine/UNHUMAN/issues/4), [#32](https://github.com/unhuman-engine/UNHUMAN/issues/32), [#35](https://github.com/unhuman-engine/UNHUMAN/issues/35) |
| 5 | Renderer and materials | glTF correctness (alpha, cull, sRGB, normal, AO, node transforms), culling, instancing, shadows, IBL, post, then extended PBR and KTX2 | [#29](https://github.com/unhuman-engine/UNHUMAN/issues/29), [#25](https://github.com/unhuman-engine/UNHUMAN/issues/25), [#7](https://github.com/unhuman-engine/UNHUMAN/issues/7), [#36](https://github.com/unhuman-engine/UNHUMAN/issues/36), [#42](https://github.com/unhuman-engine/UNHUMAN/issues/42), [#34](https://github.com/unhuman-engine/UNHUMAN/issues/34), [#43](https://github.com/unhuman-engine/UNHUMAN/issues/43) |
| 6 | Assets and audio | Registry and stable IDs, async load on jobs, VFS with scenes storing IDs, dependency graph, audio voices and buses, cooked runtime format | [#38](https://github.com/unhuman-engine/UNHUMAN/issues/38), [#45](https://github.com/unhuman-engine/UNHUMAN/issues/45), [#24](https://github.com/unhuman-engine/UNHUMAN/issues/24) |
| 7 | Editor, UI, animation, scripting | Entity hierarchy, editor ergonomics (console, undo/redo, asset browser, play-in-editor), runtime UI, animation systems, Lua and sol2 | [#17](https://github.com/unhuman-engine/UNHUMAN/issues/17), [#28](https://github.com/unhuman-engine/UNHUMAN/issues/28), [#24](https://github.com/unhuman-engine/UNHUMAN/issues/24), [#41](https://github.com/unhuman-engine/UNHUMAN/issues/41), [#44](https://github.com/unhuman-engine/UNHUMAN/issues/44) |
| 8 | CI/CD | Tier-0 gates and caching, sanitizer matrix and unit tests, Android compile gate, validation-strict GPU job with golden images, release and nightly | [#9](https://github.com/unhuman-engine/UNHUMAN/issues/9), [#10](https://github.com/unhuman-engine/UNHUMAN/issues/10), [#39](https://github.com/unhuman-engine/UNHUMAN/issues/39) |

Dropped 2026-09-24: the standalone **FrameGraph frontend** stage (was #6, issue #33). It
duplicated dependency tracking and put two graph layers between feature code and the
backend; feature code now declares passes into the RenderGraph (stage 4) directly. It can
be layered on later if renderer code ever needs a stable API that outlives backend changes.

Cross-cutting at any time: documentation [#12](https://github.com/unhuman-engine/UNHUMAN/issues/12)
and code cleanups [#40](https://github.com/unhuman-engine/UNHUMAN/issues/40).

Rows 1 and 2 are the current focus. Rows 3 through 6 are the renderer spine;
rows 7 through 9 can proceed in parallel once sync and threading are stable.

Not yet tracked: audio voices and buses (only partly covered by
[#24](https://github.com/unhuman-engine/UNHUMAN/issues/24)) and editor ergonomics
have no dedicated issue.
