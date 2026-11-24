# Robotics training and the client/server split

How UHE is meant to train drones, cars, robots and NPCs without a heavy machine on
the desk: the simulation and the training run on a machine that has the muscle
(server, workstation, cloud), the editor and the viewer stay thin (client).

This document is a **design**, not a status report. Anything claimed as
implemented below names the file it lives in; everything else is planned.

## What the tree already gives us

| Piece | Where | Note |
|---|---|---|
| ECS scene with copy/reset semantics | `UHE/src/UHE/Scene/Scene.{h,cpp}` | `Scene::Copy` already used for play-mode; a multi-env server is N scenes |
| 3D physics (Jolt) with bodies, colliders, sensors flags | `UHE/src/UHE/Physics/PhysicsSystem3D.{h,cpp}` | `GetJoltPhysicsSystem()` / `GetBodyInterface()` are exposed, so raycasts are reachable |
| 2D physics (Box2D) | `Scene::OnUpdateRuntimeSimulation` | `b2World_Step` with substeps |
| Jobs + task graph | `UHE/src/UHE/Jobsystem/` | used by physics and by the render graph |
| Frame graph + offscreen rendering | `UHE/src/Platform/Vulkan/RenderGraph/`, editor viewport framebuffer | the scene already renders to a framebuffer, not straight to the swapchain |
| Projects and per-project assets | `UHE/src/UHE/AssetsManager/Project.{h,cpp}`, `docs/architecture/assets-and-audio.md` | a scene + its assets are one unit to ship to a server |
| Headless-capable tests, ASan CI | `tests/**`, `.github/workflows/sanitizers.yml` | the harness style (real TUs, no GPU) is how the sim layer should be tested |
| Game/app shell | `UHEGAME/`, `sandbox/` | "client full base": a real application on top of the engine |

## What is missing (and is the actual work)

| Gap | Why it blocks training | Cheapest honest fix |
|---|---|---|
| No headless mode | `Application` always creates a GLFW window (`Application.cpp:31`) and `VulkanDevice` requires a `SwapchainDesc` (`VulkanDevice.h:36`) | a window-less application mode + a device created without a swapchain; render into the existing framebuffer path only when asked |
| No step/reset/observe API | nothing can drive an episode | a small `SimSession` API over `Scene`: `Reset()`, `Step(actions)`, `Observe()` |
| No sensor API | no observations to learn from | wrap Jolt's `NarrowPhaseQuery::CastRay` (LiDAR/depth/contact) and read back the camera attachment (RGB/depth) |
| Variable timestep | results differ per machine | fixed-step accumulator in the sim loop (`Scene` currently uses the frame `Timestep`) |
| No transport | client and server cannot talk | one flat binary message format over a local socket first, TCP later; no third-party dependency |
| No training-side API | the trainer lives in Python | a thin client library in `python/` speaking that protocol (no pybind needed for a socket protocol) |
| No parallel envs | one env per process is wasteful | N `Scene` instances per server, stepped in the job system, one observation buffer each |

## The split: what runs where

Three deployment shapes, same engine code:

```
1. standalone (today)      editor ── sim ── render ── ui           one process, one machine
2. thin client + server    viewer/editor ──(state, commands)── server: sim + sensors + training
3. headless trainer        trainer script ──(step/observe)── server: sim + sensors
```

Rules that keep it simple:

- **The server owns truth.** Physics, sensors, episode logic and reset live there.
  The client never simulates authoritatively.
- **The client renders.** State replication (transforms, entity spawn/despawn,
  sensor streams for display) is orders of magnitude cheaper than shipping pixels,
  and it is what the editor already knows how to draw.
- **Pixel streaming is a fallback, not the plan.** If a client cannot run the
  renderer at all (phone, browser, a machine without the needed Vulkan tier), the
  server can encode its offscreen frames and send them. That needs a video encoder
  and a decode path on the client; it is worth doing only after 2 and 3 work.
- **Features live where they are cheap.** Anything the client can compute locally
  (camera interpolation, gizmos, UI, previews) stays on the client; anything that
  needs the simulation (sensor ground truth, collision, contacts) stays on the
  server.

## Phases

Each phase is shippable on its own and has an acceptance check that can be run in
CI or by hand.

### P0 — headless simulation (foundation)

- `Application` gains a window-less mode; `UHE_SERVER` target runs a project
  (`-project <dir>`) with no GLFW, no swapchain, no ImGui.
- Fixed-step simulation loop (accumulator, configurable rate) with the same
  `Scene` update path the editor uses.
- Acceptance: `UHE_SERVER -project projects/SimulationDrone -steps 1000` runs in CI,
  exits 0, and prints the step rate and a body-count summary; ASan job covers it.

### P1 — step / reset / observe

- `SimSession` (`UHE/src/UHE/Sim/`): owns one `Scene`, applies actions to rigid
  bodies, advances the fixed step, and fills an observation struct.
- A local transport (unix socket, length-prefixed flat messages) + a
  `python/uhe_client.py` that does `reset()`, `step(action)`, `observe()`.
- Acceptance: a Python script drives a car forward for 100 steps against the
  headless server and asserts the pose moved in the expected direction.

### P2 — sensors

- Raycast wrapper over Jolt (LiDAR scan, single ray, contact probes).
- Camera observation: render the offscreen attachment and read it back as an
  array (RGB + depth), no window involved.
- IMU/odometry derived from rigid-body velocity/angular velocity.
- Acceptance: a test drives a known scene, asserts a ray hits the wall it is aimed
  at, and that a camera observation has the expected size and a non-uniform image.

### P3 — client/server split

- The editor becomes a client: it connects to a server, receives replicated state,
  draws it locally, and sends commands (spawn, reset, step, play/pause).
- Server side: replication of transform/entity diffs at a low rate; the sensor
  streams are optional (they are training data, not view data).
- Acceptance: the editor on the laptop shows a scene simulated on another machine
  and stays responsive; disconnecting leaves the server training.

### P4 — parallel training

- N envs per server (`Scene` per env, stepped through the job system), an episode
  scheduler with automatic reset on terminal state, and a batched observation
  buffer.
- Acceptance: one server runs 16 envs of the same project and reports throughput
  per env; ASan/TSan jobs stay clean.

### P5 — games on the same base

- The client/server pair is the multiplayer base: authoritative server, predicting
  client. `UHEGAME` becomes the first client.
- Acceptance: two clients join a server-run scene and see each other move.

## Determinism, because training depends on it

- Fixed step everywhere in the sim path (P0); the render path may still run at
  frame rate.
- One physics island per env, stepped on a pinned thread count, so results do not
  depend on how many cores the machine has.
- Randomness (domain randomisation) goes through one seeded stream per env, never
  through `rand()`/global state.
- Determinism is tested, not assumed: a golden run (same seed, same actions, same
  final pose) is part of the test suite from P1 on.

## Non-goals

- Photoreal rendering, or matching Unreal's feature list. "Simple and enough" is
  the point: enough scene fidelity and sensors to train, and enough renderer to
  make a game.
- Distributed training frameworks, RL algorithm implementations, dataset tooling —
  those live in Python outside the engine.
- A browser/phone client before P3 works on desktop.

## Open questions

1. Transport: raw TCP with a flat format (no dependency) vs gRPC (tooling, but a
   dependency and a build cost). Leaning: raw first, revisit at P3.
2. Observation transport for camera sensors: shared memory on the same machine vs
   sockets over the network — decide when P2 lands.
3. Whether the server keeps a fixed set of envs per process or spawns worker
   processes — decide at P4 with measurements, not up front.
