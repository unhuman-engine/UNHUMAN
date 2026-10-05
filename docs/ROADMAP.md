# UHE Roadmap

The **work order** for the engine. The GitHub issue tracker is the *queue*; this file is the *map*.

**Verified against:** `improve_vulkan` @ `c271467` plus the uncommitted working tree, 2026-09-13.
**Rule:** status changes need evidence (a commit hash, a CI run, a file you can point at). No item
is marked done because a doc says so.

## Status vocabulary

| Status | Meaning |
|---|---|
| `DONE` | In the tree, on a merged/main-line commit |
| `IN PROGRESS` | Partially implemented, currently being worked |
| `STUB` | Files exist, or headers declare an API, but behaviour is empty and nothing calls it |
| `TODO` | Nothing written yet |
| `DESIGN` | A doc in `docs/architecture/` specifies it; no code |
| `DEFERRED` | Deliberately not now — named so it stops resurfacing |
| `UNDECIDED` | Blocked on a decision in the register at the bottom |

## Workstreams

| ID | Stream | Issues | Primary doc |
|---|---|---|---|
| W1 | Vulkan capability tiers + synchronization | #2, #27, #14 | [vulkan-sync-and-rendergraph.md](architecture/vulkan-sync-and-rendergraph.md) |
| W2 | Render graph (backend → frontend) | #4, #7 | [framegraph-and-rendergraph.md](architecture/framegraph-and-rendergraph.md) |
| W3 | Renderer features + material correctness | #29, #25 | [backlog/renderer.md](backlog/renderer.md) |
| W4 | Jobs, assets, audio | #5 | [jobsystem.md](architecture/jobsystem.md), [assets-and-audio.md](architecture/assets-and-audio.md) |
| W5 | Tooling: CI, docs, editor, animation, scripting | #9, #10, #12, #17, #24, #28 | [ci-cd.md](architecture/ci-cd.md) |

Dependency shape: **W1 → W2 → W3 (passes/shading)** because barriers must have an owner before
multi-pass work is safe. **W3 materials and W4 are independent** and can be interleaved when W1/W2
stall on a decision. W5 is parallel by nature.

---

## M0 — Hygiene & ground truth (days; do before anything else)

Nothing in W1/W2 is testable until the engine reports its own capabilities correctly, and dead
stubs make every "is this implemented?" question cost a file read.

| Task | Where | Issue | Status |
|---|---|---|---|
| Docs reorganisation: `docs/` tree, index, this roadmap, filename typo fix, stale TODO mirrors | `docs/**`, `IDEA.md`, `readme.md` | #12 | `DONE` (uncommitted) |
| Fix `IsEnable("")` in semaphore detection → timeline semaphores are **never** on today | `UHE/src/Platform/Vulkan/VulkanSemaphore.cpp:11` | #2 | `TODO` |
| `VulkanBinaryFence.h:12` re-declares `VulkanBinarySemaphore` (ODR/redefinition if both headers land in one TU) | `UHE/src/Platform/Vulkan/VulkanBinaryFence.h` | #2 | `TODO` |
| Delete or wire the dead `requiredDeviceExtension` member | `UHE/src/Platform/Vulkan/VulkanLogicalDevice.h:40-43` | #2 | `TODO` |
| Barrier `oldLayout` is hardcoded `Undefined` (illegal after first use) | `UHE/src/Platform/Vulkan/VulkanCommandBuffer.cpp:100,149,181` | #2, #4 | `TODO` |
| Delete dead stubs: `RenderGraph/JobSystem.*` (superseded by `UHE/Jobsystem`), decide on `RenderGraph/RenderGraphVulkan.*` + `ShaderSystem/VulkanShaderManager.*` | `UHE/src/Platform/Vulkan/{RenderGraph,ShaderSystem}/` | #4, #7 | `TODO` |
| CI "Now" list (§12.1–12.5 of the CI doc): concurrency+permissions+SHA pinning, caching, format/warnings gates, CTest+`UHE_TESTS`+`ci` preset, vendor CMake fixes in-tree instead of CI rewriting them | `.github/workflows/**`, `ci/` | #9, #10 | `TODO` |
| Renderer checklist truth-up — `add job system` is done (`c271467`), close it in #7 | #7, [backlog/renderer.md](backlog/renderer.md) | #7 | `TODO` |
| Tracking hygiene: `.worktrees/` ignored (done); `imgui.ini` is tracked *and* in `.gitignore` (ignore is a no-op — `git rm --cached` or drop the line) | `.gitignore` | — | `IN PROGRESS` |

**Exit:** `--force-legacy`-able build reports its real Vulkan tier; CI has unit tests; `git status`
on a fresh checkout is clean.

---

## M1 — Vulkan capability tiers & sync foundation (W1) — issues #2, #27, #14

Order is the sync doc's §5 list; don't reorder.

| Step | Deliverable | Doc ref | Status |
|---|---|---|---|
| 1 | Capability ladder from the device API version *and* extensions with core-promotion awareness (`Sync2`, `Timeline`, `DynamicRendering`, `Compute`), features requested via version structs **xor** KHR structs — never both for one bit | sync §1.1, §2.2 | `TODO` |
| 2 | `VulkanBarrierEncoder` + `VulkanSubmitEncoder`; re-express `VulkanCommandBuffer::Begin/EndRenderPass` and `VulkanDevice::Begin/End` on top of them. Must be **behaviour-identical on modern GPUs** | sync §2.3–§2.6, framegraph Phase 1–2 | `TODO` |
| 3 | `VulkanSyncTier::Legacy` path + `--force-legacy` toggle for testing the Android-shaped route | sync §2.2, §3.7 | `TODO` |
| 4 | CI leg that runs the renderer with Sync2 disabled (Lavapipe). This is the Android safety net | sync §5 step 3 | `TODO` |
| 5 | Real timeline semaphores + deferred destruction keyed on GPU progress | sync §2.4, §2.7 | `TODO` |
| 6 | Descriptor frontend refactor: bind the global set once, hand per-pass sets through the pass context instead of letting passes reach into `VulkanDescriptorManager` | #14, sync §6 | `TODO` |

**Exit:** one scene renders identically under both tiers, CI covers both, and no barrier uses an
untracked `oldLayout`.

---

## M2 — Render graph backend (W2) — issues #4, #7

| Step | Deliverable | Doc ref | Status |
|---|---|---|---|
| 1 | Resource-state tracking in `ResourcePool` (real image layouts, still no full graph) | sync §3.5 | `TODO` |
| 2 | `RenderGraphTypes/Builder/Compiler` — topological sort, dead-pass cull, barrier synthesis, single queue, **dynamic rendering only** to start | sync §3.2–§3.4 | `TODO` |
| 3 | `RenderGraphExecutorVulkan` on the existing `TaskGraph`; delete `RenderGraph/JobSystem.*`; migrate the shadow pass, then opaque, then lighting — anything unmigrated runs as one "legacy blob" node in the same DAG | sync §3.6, framegraph Phase 3 | `TODO` |
| 4 | Pipeline state caches for render-pass and fallback paths | sync §5 step 6, #7 | `TODO` |
| 5 | Multithreaded secondary command-buffer recording (needs per-thread command pools — `ImmediateSubmit` currently resets one shared pool) | sync §2.8 | `TODO` |
| 6 | Memory aliasing + cross-queue edges | sync §3.5, framegraph Phase 5 | `DEFERRED` (after lifetimes are stable) |

**Exit:** ≥2 passes execute through the graph, validation-clean, single submit path.

---

## M3 — Renderer features & materials (W3) — issues #29, #25

Do #29 **before** the #25 wishlist: it is small, it is mostly *reading fields fastgltf already
parsed*, and it fixes visible wrongness (foliage renders black).

| Step | Deliverable | Issue | Status |
|---|---|---|---|
| 1 | glTF correctness first pass: `alphaMode`+`alphaCutoff`, `doubleSided`→pipeline cull mode, `baseColorFactor`, sRGB-correct baseColor sampling, normal/AO/emissive textures, `node.transform` in `ProcessNode` (currently ignored → multi-node models collapse) | #29 | `TODO` |
| 2 | Frustum culling (AABB/sphere vs camera frustum) before `Renderer3D::SubmitModel` | #25 §1 | `TODO` |
| 3 | Instanced rendering (`DrawIndexedInstanced`) for identical meshes | #25 §1 | `TODO` |
| 4 | Pass splitting via the graph: depth prepass → opaque → transparent → post | #25 §1, M2 | `TODO` |
| 5 | Shadows: directional CSM, then point-light cubemaps | #25 §2 | `TODO` |
| 6 | IBL: skybox + irradiance/prefilter generation; then analytical area lights in `Basic3D.slang` | #25 §2 | `TODO` |
| 7 | Post stack: ACES tonemap → bloom → FXAA | #25 §4 | `TODO` |
| 8 | Extended PBR: clearcoat, transmission/volume, sheen, iridescence, anisotropy; KTX2/meshopt loaders (Draco still `TODO`: needs the vendored decoder) | #29 tiers 2–4, #42, #25 §3 | `IN PROGRESS` |
| — | Terrain system, water/SSR shader | #25 §4 | `DEFERRED` |

**Exit per step:** a sandbox scene exists that shows the feature, and the PR says which scene.

---

## M4 — Jobs, assets, audio (W4) — issue #5

| Step | Deliverable | Doc ref | Status |
|---|---|---|---|
| 1 | Per-thread job queues + work-stealing deques (kill the single hot-path mutex) | jobsystem §10.1 | `TODO` |
| 2 | Job continuations (child jobs instead of OS-stack recursion) | jobsystem §10.2 | `TODO` |
| 3 | `ParallelFor` cache-line-aligned batch hints | jobsystem §10.5 | `TODO` |
| 4 | Asset Phase A: fix `UUID` uniqueness, `AssetManager` registry + path→ID + refcount + sync `Load`, migrate model/texture/audio loads. Renames while churn is cheap: `AssestsManager`→`AssetManager`, `UIID`→`UUID` | assets §2.1–§2.3, §5A | `TODO` |
| 5 | Asset Phase B: `IAssetLoader` (worker load + main-thread finalize), `LoadAsync` on the Jobsystem, `AssetManager::Update` delivery; audio decoded-sound cache, `Voice` handles, master volume, auto listener | assets §2.5, §3.1–§3.2, §5B | `TODO` |
| 6 | Asset Phase C: mount-based VFS, scenes serialize `AssetID` with path fallback | assets §2.8, §2.10, §5C | `TODO` |
| 7 | Asset Phase D: dependency graph via `TaskGraph`, audio buses/ducking, event assets | assets §2.6, §3.4, §5D | `TODO` |
| — | Cook/packaging pipeline, mod override layering, music streaming, hot reload | assets §2.7, §2.9, §5E | `DEFERRED` |

**Exit (A/B):** loading the same asset twice returns one instance; a level preloads off-thread
without stalling frames; rapid shots don't hitch.

---

## M5 — Render graph completeness — issues #4, #7

Only the RenderGraph layer. The separate FrameGraph frontend was dropped on
2026-09-24: it duplicated dependency tracking and renderer code would have to cross two
graph layers to add a pass. Feature code will declare passes directly into the render
graph instead. Revisit only if renderer code needs a stable API that outlives backend
changes — not before the render graph itself works.

| Step | Deliverable | Status |
|---|---|---|
| 1 | RenderGraph builder + resource tracking: `VulkanRenderGraphBuilder`/`Resources`/`Types` get real bodies | `TODO` |
| 2 | RenderGraph compiler: barriers, pass ordering, layouts from the graph | `TODO` |
| 3 | RenderGraph executor: command list partitioning and submission | `TODO` |
| 4 | Feature code declares passes ("shadow", "gbuffer", "lighting", "tonemap") instead of calling `Renderer3D::SubmitModel` directly | `TODO` |
| 5 | Async compute / second queue: passes declare queue hints from the start even if the compiler ignores them | `DEFERRED` |
| 6 | Graph validation + debug overlays (which pass produced this texture) | `DEFERRED` |
| — | Separate FrameGraph frontend (logical resources, `FGHandle<T>`, blackboard, graph caching) — dropped, can be layered on later if ever needed | `CANCELLED` |

**Exit:** adding a pass means declaring it, not hand-writing barriers.

---

## M6 — Editor, animation, scripting (W5) — issues #17, #24, #28

| Step | Deliverable | Issue | Status |
|---|---|---|---|
| 1 | Entity hierarchy v2: root/children in entt + editor tree (today a model entity has exactly one parent, no root-child concept) | #17 | `TODO` |
| 2 | Animation & audio visualization — needs a lib decision (OpenTimelineIO is a *video editor* format; check whether it fits before committing) | #24 | `UNDECIDED` |
| 3 | Scripting host — Lua is the working proposal; scope is ECS binding + hot reload, no native FFI breadth | #28 | `UNDECIDED` |

---

## W5 CI/CD ladder — issues #9, #10

From [ci-cd.md](architecture/ci-cd.md) §12, in its own order:

| When | Items | Status |
|---|---|---|
| Now (hours) | `concurrency`+`permissions`+SHA pinning, Dependabot+actionlint, caching (ccache/Slang/Vulkan SDK), `clang-format`+warnings-as-errors jobs, `UHE_TESTS`+CTest+`ci` preset, move Jobsystem tests in-tree, fix vendor CMake in-tree | `TODO` |
| Next (days) | UBSan + ASan/UBSan matrix, convert `test-renderer.yml` off `timeout … || [ $? -eq 124 ]`, TSan on a Vulkan-free unit binary, Linux-Clang + Windows ctest + macOS build | `TODO` |
| Then (week+) | Android compile gate (protects the 1.1 fallback), validation-strict GPU job + golden images, self-hosted GPU runner, release-on-tag + license bundle, nightly soak | `DEFERRED` |

Known concrete defects the CI doc §13 already lists: deprecated `UHE_ENABLE_ASAN`, workflows
rewriting `UHE/vendor/imgui/CMakeLists.txt` instead of fixing it in-repo, TSan suppressing the whole
Vulkan loader, Release-only Linux build (no assertions compiled anywhere), no `fail-fast: false`.

---

## Issue index (all open items, 2026-09-13)

| Issue | Title | Stream | Milestone |
|---|---|---|---|
| [#2](https://github.com/unhuman-engine/UNHUMAN/issues/2) | Feature detection for Vulkan 1.1 → 1.4 | W1 | M0/M1 |
| [#27](https://github.com/unhuman-engine/UNHUMAN/pull/27) | **PR** Vulkan sync2 fallback setup | W1 | M1 |
| [#14](https://github.com/unhuman-engine/UNHUMAN/issues/14) | Descriptor set implementation in the frontend | W1 | M1 |
| [#4](https://github.com/unhuman-engine/UNHUMAN/issues/4) | RenderGraph in the Vulkan backend | W2 | M2/M5 |
| [#7](https://github.com/unhuman-engine/UNHUMAN/issues/7) | TODO for renderer (checklist mirror) | W2/W3 | M0–M2 |
| [#29](https://github.com/unhuman-engine/UNHUMAN/issues/29) | glTF/GLB material + extension gaps (black foliage) | W3 | M3.1 |
| [#25](https://github.com/unhuman-engine/UNHUMAN/issues/25) | Renderer feature list (culling → terrain) | W3 | M3 |
| [#5](https://github.com/unhuman-engine/UNHUMAN/issues/5) | Multithreading | W4 | M4 |
| [#9](https://github.com/unhuman-engine/UNHUMAN/issues/9) | Add ASan | W5 | CI ladder |
| [#10](https://github.com/unhuman-engine/UNHUMAN/issues/10) | CI/CD checks (image comparison still open) | W5 | CI ladder |
| [#12](https://github.com/unhuman-engine/UNHUMAN/issues/12) | Documentation for rendering + architecture | W5 | this pass + M5 |
| [#17](https://github.com/unhuman-engine/UNHUMAN/issues/17) | Entity child nodes / hierarchy | W5 | M6.1 |
| [#24](https://github.com/unhuman-engine/UNHUMAN/issues/24) | Animation + audio visualization | W5 | M6.2 |
| [#28](https://github.com/unhuman-engine/UNHUMAN/issues/28) | Scripting language (Lua) | W5 | M6.3 |

Delivered already, so it doesn't get re-litigated: #26/#18/#8 text rendering (MSDF), #23 sandbox
AimLab game, #22/#20 audio (miniaudio), #19/#10-partial headless CI + sanitizers, #16/#15
CONTRIBUTING, #11/#6 descriptor full + fallback setup, #3 Vulkan 1.1→1.4 refactor, #21 sandbox game,
#17-adjacent editor QoL in `game_change`, #1 ImGui mouse release (old repo).

---

## Decision register

Answer these in the doc that owns them, then mark them `DECIDED` here. Anything `UNDECIDED` is a
stop-work signal for its milestone.

| # | Decision | Owner doc | State |
|---|---|---|---|
| D1 | One frame timeline vs per-queue timelines | sync §6 | `UNDECIDED` — start: one frame timeline + binary semaphores for cross-queue |
| D2 | How aggressively to split passes for parallel recording | sync §6 | `UNDECIDED` — measure first |
| D3 | Graph cache key (stable slots for shadows/clusters) | sync §6 | `DEFERRED` — no RenderGraph to cache until M5 |
| D4 | Does `RenderGraph` stay inside `RHI::VULKAN` or move to `UHE/RHI` | sync §6 | `UNDECIDED` — stay in Vulkan until a second backend is real |
| D5 | Descriptor ownership: global set once + optional per-pass sets | sync §6 | `UNDECIDED` |
| D6 | Where FrameGraph lives (`UHE/RHI/FrameGraph/`) | — | `CANCELLED` 2026-09-24 — separate FrameGraph layer dropped; feature code declares passes into the RenderGraph directly |
| D7 | One graph per frame vs per view | sync §6 | `UNDECIDED` — handles are view-agnostic now |
| D8 | When the graph is rebuilt (define the hash inputs) | sync §6 | `DEFERRED` — follow D3 |
| D9 | Async compute ambition (queue hints from the start?) | sync §6 | `UNDECIDED` |
| D10 | Asset handle lifetime: strong `Ref` vs weak + pin | assets §6 | `UNDECIDED` — start strong |
| D11 | Asset ID: random-at-import vs content hash | assets §6 | `UNDECIDED` |
| D12 | Audio API threading: main-thread-only vs command queue | assets §6 | `UNDECIDED` — main-thread-only recommended |
| D13 | Test framework: doctest vs Catch2 | ci §14 | `UNDECIDED` |
| D14 | Self-hosted GPU runner: yes/no (decides whether correctness stays Lavapipe-only) | ci §14 | `UNDECIDED` |
| D15 | Android ambition: compile gate only, or device smoke | ci §14 | `UNDECIDED` — compile gate is the high-value one |
| D16 | clang-tidy appetite: changed-files baseline vs repo-wide + suppressions | ci §14 | `UNDECIDED` |
| D17 | Animation/audio visualization library | #24 | `UNDECIDED` |
| D18 | Scripting language + binding scope (Lua proposal) | #28 | `UNDECIDED` |
| D19 | Solar/AMD-specific low-end floor: Vega 7 (GFX9) must stay a supported target | IDEA.md | `DECIDED` — 1.1/1.2 must work on it |

---

## Keeping this file honest

1. Closing an issue updates its row **and** the issue index here.
2. A PR body names the roadmap row it closes (`Roadmap: M1 step 2`).
3. Statuses change only with evidence — commit hash, CI link, or a file that proves it.
4. Monthly: re-walk the defect lists (sync §1.2 style) against the tree. They are line-numbered
   snapshots and rot after the first refactor.
5. If a task cannot be pointed at a file path, it is not a task yet — it belongs in the register.
