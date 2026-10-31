# Documentation index

Everything under `docs/` is prose; everything under `UHE/src/` is code. Keeping
the two separate means the docs stay greppable and are not moved around by
refactors.

Last verified: 2026-09-13 against `improve_vulkan` at `c271467` plus the
working tree.

## Map

| Path | Contents |
|---|---|
| [../IDEA.md](../IDEA.md) | Project idea, scope and direction |
| [ROADMAP.md](ROADMAP.md) | Work order, milestones, issue map and decision register |
| [WORK_ORDER.md](WORK_ORDER.md) | Short sequential list of what comes next |
| [architecture/](architecture/) | Designs for subsystems that are not fully implemented |
| [backlog/renderer.md](backlog/renderer.md) | Vulkan renderer status, mirroring issue #7 |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | Code style, workflow and commit conventions |
| [../readme.md](../readme.md) | Public overview and build instructions |

## Architecture documents

| Document | Subject | Status | Issues |
|---|---|---|---|
| [vulkan-sync-and-rendergraph.md](architecture/vulkan-sync-and-rendergraph.md) | Sync2 and legacy fallback tiers, barrier and submit encoders, render-graph backend, and the defects to fix first | DESIGN | #2, #27, #4, #14 |
| [framegraph-and-rendergraph.md](architecture/framegraph-and-rendergraph.md) | FrameGraph frontend versus RenderGraph backend — **SUPERSEDED 2026-09-24**, the separate frontend layer is dropped; keep only the RenderGraph backend | ARCHIVED | #4, #14 |
| [jobsystem.md](architecture/jobsystem.md) | The job-system thread pool and the TaskGraph DAG executor | IMPLEMENTED at `c271467` | #5 |
| [assets-and-audio.md](architecture/assets-and-audio.md) | Asset identity, refcounting and async loading, and the audio voice and bus model | DESIGN, phases A and B not started | #24 |
| [ci-cd.md](architecture/ci-cd.md) | CI tiers, sanitizers, test infrastructure and rollout order | PARTIAL, builds, sanitizers and `ctest -L unit` run; the validation gate is enforced in `sanitizers.yml` and `test-renderer.yml` | #9, #10 |
| [scripting.md](architecture/scripting.md) | Lua 5.4 with sol2: VM, bindings, ECS integration, sandbox and hot reload | IMPLEMENTED, phase 1 + hot reload; coroutines and reflection-driven component access still open | #28 |
| [shaders.md](architecture/shaders.md) | Offline Slang → `.spv`, SPIR-V reflection, and hot reload | IMPLEMENTED, covered by `tests/shader` | #29, #7 |
| [robotics-and-clients.md](architecture/robotics-and-clients.md) | Headless simulation, step/reset/observe, sensors, and the thin-client/server split for training robots, drones and cars | DESIGN | — |

## Code state the documents assume

| Claim | Evidence |
|---|---|
| The job system and task graph exist and work | `UHE/src/UHE/Jobsystem/{Jobsystem,Taskgraph}.{h,cpp}`, commit `c271467` |
| The render-graph backend is an empty stub | `UHE/src/Platform/Vulkan/RenderGraph/RenderGraphVulkan.{h,cpp}` has no bodies and no callers |
| The shader manager is an empty stub | `UHE/src/Platform/Vulkan/ShaderSystem/VulkanShaderManager.{h,cpp}` declares `Init` and `ShutDown` with no callers |
| Stub sources are compiled into the binary | `UHE/CMakeLists.txt:6` uses a recursive glob over `UHE/src` |
| The compute pipeline object is unused | `VulkanComputePipeline.{h,cpp}` is referenced nowhere else |
| The descriptor backend is modernized but the frontend is not | issue #14 |
| There are unit tests in-tree | `tests/` holds one self-contained harness per subsystem, registered with CTest and run by `ctest -L unit` (see [ci-cd.md](architecture/ci-cd.md)) |

## Conventions

1. Filenames are lowercase with hyphens, spelled correctly, one topic per file.
2. Every document opens with a status block that names related issues and where
   the code will land, so a reader knows in a few seconds whether it describes
   reality or intent.
3. Status vocabulary is fixed: `DONE`, `IN PROGRESS`, `STUB`, `TODO`, `DESIGN`,
   `DEFERRED`, `DECIDED` and `UNDECIDED`.
4. The issue tracker is the queue and `ROADMAP.md` is the map. Work items get an
   issue, and the roadmap row links it.
5. `file:line` references are snapshots against a specific commit and should be
   re-verified before being acted on.
6. Decisions belong in the register at the bottom of `ROADMAP.md`, not in chat.

## Documents still to write

Tracked as #12:

- `architecture/rendering-pipeline.md`, how a frame flows today from draw list to
  RHI to Vulkan, for people landing pull requests. The existing documents
  describe where it should go rather than where it is.
- `architecture/ecs-and-scene.md`, covering entt usage, scene serialization and
  the entity hierarchy, related to #17.
- `dev/testing.md`, how to run the headless renderer smoke and the sanitizer
  builds, and later CTest.
- `dev/build.md`, covering presets, `CMakePresets.json`, sanitizer builds and the
  Slang and vendor fetch, which currently live only in `readme.md` and the CI
  YAML.
