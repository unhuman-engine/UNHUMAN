# Moving 3D Scene Rendering onto the Render Graph

> **Status:** DRAFT — for raj's review. Nothing here has been implemented.
> **Author context:** written during the glTF asset work (branch `asset-improvement`),
> immediately after fixing the "model never submitted" bug. The engine is stable right
> now, which is the right time to take this on, but the ordering matters: the scene
> pass has to exist before asset work hits another wall.

---

## Goal

Make `Renderer3D` record its work **inside a render-graph pass** instead of directly
onto the frame's primary command buffer, so the render graph actually carries scene
rendering and not just the ImGui pass.

Immediate (legacy) submit **stays** — the user was explicit about this and it's a
legitimate escape hatch. This plan adds a path; it does not remove one.

---

## Current state (verified, not assumed)

Findings below were read out of the tree on 2026-10-01. Line numbers drift — re-check
before relying on them.

### The render graph is real and runs every frame

`VulkanDevice::EndFrameGraph()` (`UHE/src/Platform/Vulkan/VulkanDevice.cpp:579`) does,
per frame:

```cpp
RGCompileResult compiled = m_FrameGraph.Compile();
RGResolvedFrame resolved   = m_RenderGraphExecutor.Resolve(compiled.frame, ..., resolveErrors);
m_PassNodes = m_RenderGraphExecutor.MapToTaskgraph(m_TaskGraph, resolved, ..., cmd, m_BarrierEncoder);
m_RenderGraphExecutor.ExecuteGraph(m_TaskGraph, m_Jobsystem, m_PassNodes);
m_FrameGraph.Reset();
```

Called from `VulkanDevice::End()` (line ~434), skipped when `m_FrameSkipped`.

### But the graph contains exactly one pass

The **only** `AddPass` call site in the entire codebase outside `RenderGraph/` is:

```
UHE/src/Platform/Vulkan/VulkanDevice.cpp:551   m_FrameGraph.AddPass(name, RGPassType::Graphics)   // ImGui
```

The comment in `EndFrameGraph` referring to *"AimLab's scene pass"* is stale — no such
pass is declared in this tree. So the rendergraph tests validate a **single-pass graph
containing only ImGui**: compilation, barrier resolution and taskgraph mapping are all
exercised, but never against real scene rendering.

### `Renderer3D` never touches the graph

`UHE/src/UHE/Renderer3D/Renderer3D.cpp` has zero references to `AddPass` or
`RGPassContext`. It records directly:

- `BeginScene(...)` (line 174 / 187) — sets `ViewProjection`, `CameraPosition`, uploads
  the light storage buffer, resets `BoneBufferOffset`. **Opens no render scope.**
- `EndScene()` (line 201) — **empty function body.**
- `DrawGrid()` (line 203) — `cmd.BindPipeline` / `PushConstants` / `cmd.Draw(6, 0)`.
- `SubmitModel(...)` (line 223) — `cmd.BindPipeline` / `PushConstants` / `cmd.Draw`.

The only render scope opened anywhere is ImGui's, at
`UHE/src/Platform/Vulkan/UI/VulkanImGuiBackend.cpp:111`.

**Consequence:** depth testing for the 3D scene depends on a render pass opened by
someone else. That happens to work today, and it is fragile.

### The graph API already supports what a scene pass needs

`RGPassBuilder` (`UHE/src/Platform/Vulkan/RenderGraph/VulkanRenderGraphBuilder.h`)
already exposes `Read()`, `Write()`, `Color()`, `Depth()`.

`RGPassContext` (`.../VulkanRenderGraphExecutor.h:106`) exposes `View()`, `Buffer()`,
`Extent()`, `Cmd()`, `Dispatcher()`, `PassIndex()` — and deliberately **no barrier API**;
the compiler guarantees correct state on `View()`/`Buffer()`.

So the extension point is genuinely one function (`AddPass`). The missing piece is
scene-side plumbing, not graph capability.

### Constraints that shape the work

1. **Passes execute after `End()` calls `EndFrameGraph()`.** Today `SubmitModel` records
   the instant it is called from the scene walk. Moving it into a graph pass means the
   submit calls become **deferred** — collected during the frame, replayed inside the
   pass callback. This changes `Renderer3D`'s contract; it is not just a different call
   site.
2. **Ordering is currently constrained.** `EndFrameGraph` chains every pass to its
   predecessor (`deps.push_back(previous)`) so recording order is preserved, because all
   passes record into the same primary buffer. A scene pass declared before ImGui will
   therefore record before ImGui — which is what we want, but it's an existing
   constraint to respect rather than fight.
3. **Push constants are at their ceiling.** `PushConstants` is now **256 bytes**, exactly
   `maxPushConstantsSize` on this device (verified via `vulkaninfo`, GFX9). Any new
   per-draw data must go into a descriptor, not a push constant.
4. **`m_FrameGraphFailed` latches.** Set at `VulkanDevice.cpp:584` (dynamic rendering
   unsupported), `:600` (compile failure), `:617` (resolve failure). `End()` has a legacy
   fallback path at line ~438. **A compile error in a new scene pass would silently drop
   the entire scene with only a log line** — the exact silent-failure shape this session
   already hit three times.
5. **Swapchain import exists; nothing else does.** `ImportTexture("Swapchain", ...)` at
   line 545 is the only import in the tree. Offscreen color/depth targets would be new.
6. **No MSAA anywhere in the renderer today.** `sampleCount` plumbing exists in
   `VulkanPipelineState.cpp` / `VulkanRenderPass.cpp` but no scene usage.

---

## Approach

Two stages. Stage 1 is the whole point and is independently shippable; Stage 2 is
optional polish that can be deferred indefinitely.

### Stage 1 — scene pass resolving to the swapchain (do this first)

One graphics pass that owns a depth attachment, records the grid and every model, and
writes straight to the swapchain image. This is the smallest change that makes the graph
carry real scene rendering.

Why resolve to swapchain rather than offscreen first: fewer moving parts, and it is
verifiable. Post-processing needs an offscreen target, but nothing here does — YAGNI until
something needs it.

### Stage 2 — offscreen HDR + depth + optional post (deferred)

Only if post-processing, TAA, or bloom become requirements.

---

## Task list

### Task 1: Capture the scene's draw list instead of recording immediately

**Why first:** everything else depends on deferred recording existing. Doing this alone
changes nothing visually — it is a pure refactor with immediate-submit still as the
consumer — which makes it independently revertible if the rest goes badly.

**Files:**
- Modify: `UHE/src/UHE/Renderer3D/Renderer3D.h`
- Modify: `UHE/src/UHE/Renderer3D/Renderer3D.cpp`
- Test: `tests/rendergraph/` (extend; see Task 5)

**Step 1: Write the failing test**

Add to `tests/rendergraph/main.cpp` a check that a scene submit is *recorded*, not
executed, when the graph path is inactive:

```cpp
// Renderer3D must be able to defer a submit. With no graph pass active the
// draw list stays pending rather than recording into the command buffer.
REQUIRE(Renderer3D::HasPendingDraws() == false);   // fresh frame, nothing submitted
```

**Step 2: Run test to verify failure**

```bash
cd /home/misa/coderepo/enginedev/unhuman
cmake --build build_debug --target rendergraph_test -j8
./bin/Debug-linux-x64/rendergraph_test
```
Expected: compile error — `HasPendingDraws` does not exist.

**Step 3: Write minimal implementation**

```cpp
// Renderer3D.h
struct PendingDraw
{
    const RD3d::Model* model;
    glm::mat4 transform;
    int entityID;
    const RD3d::Animator* animator;
};

// Immediate submit stays available. When false, SubmitModel appends to the
// draw list instead of recording; RecordScene replays it inside a graph pass.
static void SetDeferredSubmit(bool deferred);
static void SubmitModel(const RD3d::Model& model, const glm::mat4& transform = glm::mat4(1.0f),
                        int entityID = -1, const RD3d::Animator* animator = nullptr);
static void RecordScene(RHI::CommandBuffer& cmd);   // replays the pending list
static bool HasPendingDraws();
static void ClearPendingDraws();
```

In `Renderer3D.cpp`, split the existing body of `SubmitModel` so the draw-call portion
becomes `RecordModel(RHI::CommandBuffer&, const PendingDraw&)`, called either immediately
(current behaviour) or from `RecordScene`.

**Step 4: Run test to verify pass**

Expected: `rendergraph_test` green; `ctest -L unit` still 3/3.

**Step 5: Verify nothing regressed**

```bash
cmake --build build_debug -j8
timeout 25 ./bin/Debug-linux-x64/UHE_EDITOR > /tmp/e.log 2>&1
grep -cE "VUID|validation error|\[error\]" /tmp/e.log    # expect 0
```
Confirm the model still renders by loading one via drag-drop. **This is the gate** —
Task 1 must be visually indistinguishable from current behaviour.

**Step 6: Commit** (only after asking raj — standing rule)

```bash
git add UHE/src/UHE/Renderer3D/Renderer3D.{h,cpp} tests/rendergraph
git commit -m "renderer3d: capture scene draws into a deferrable list"
```

---

### Task 2: Give `Renderer3D` a depth attachment it owns

**Why:** `EndScene()` is empty and no scope is opened. A graph pass needs a declared
depth attachment; the current implicit dependence on ImGui's scope is the fragility.

**Files:**
- Modify: `UHE/src/UHE/Renderer3D/Renderer3D.cpp` (`Init`, `Shutdown`)
- Modify: `UHE/src/UHE/Renderer/Framebuffer.{h,cpp}` if the spec struct needs a field

**Step 1: Write the failing test**

`tests/rendergraph`: a scene pass declaring `Depth()` with no depth target registered
must produce a named resolve error, not a silent skip.

```cpp
RGResolvedFrame resolved = executor.Resolve(compiled.frame, specs, errors);
REQUIRE_FALSE(errors.empty());
REQUIRE(errors[0].find("depth") != std::string::npos);
```

**Step 2: Run — expect failure** (no depth validation exists yet).

**Step 3: Implement**

Create the depth texture in `Renderer3D::Init()` as `DEPTH24STENCIL8` (matches the
existing `Framebuffer.h:21` choice), sized to the framebuffer extent, recreated on
resize. Expose the `RGTextureHandle` + `TextureHandle` pair so the device can register it.

**Step 4: Run — expect pass.**

**Step 5: Verify**

```bash
cmake --build build_debug -j8
timeout 25 ./bin/Debug-linux-x64/UHE_EDITOR > /tmp/e.log 2>&1
grep -cE "VUID|validation error" /tmp/e.log    # expect 0
```
Depth-correct rendering must be unchanged.

**Step 6: Commit** (ask first)

---

### Task 3: Declare the scene pass and record inside it

**Why:** the actual adoption. Graph gains a real scene pass.

**Files:**
- Modify: `UHE/src/UHE/Renderer3D/Renderer3D.cpp` (new `DeclareScenePass`)
- Modify: `UHE/src/Platform/Vulkan/VulkanDevice.cpp` (expose swapchain RG handle earlier,
  or add `RegisterScenePass`)
- Modify: `UHE/src/Platform/Vulkan/VulkanImGuiBackend.cpp` (**order matters** — see below)

**Step 1: Write the failing test**

`tests/rendergraph`: a two-pass graph (scene → imgui) compiles, and the scene pass
resolves its color + depth attachments without error.

```cpp
VulkanRenderGraph graph;
auto scene = graph.AddPass("Scene", RGPassType::Graphics);
scene.Write(color).Color({...}).Depth({...}).Execute([](RGPassContext&) {});
auto ui = graph.AddPass("ImGui", RGPassType::Graphics);
ui.Write(swap).Color({...}).Execute([](RGPassContext&) {});
REQUIRE(graph.Compile().Ok());
```

**Step 2: Run — expect failure** (two-pass graphs untested).

**Step 3: Implement**

In `Renderer3D`, add:

```cpp
void Renderer3D::DeclareScenePass()
{
    if (!m_ScenePassDeclared)
    {
        auto& device = VulkanDevice::Get();  // or however the frame graph is reached
        auto& graph = device.GetFrameGraph();
        auto& pass = graph.AddPass("Scene", RGPassType::Graphics);
        pass.Write(swap)
            .Color({swap, LoadOp::Load, StoreOp::Store, {0, 0, 0, 1}})
            .Depth({depth, LoadOp::Clear, StoreOp::Store, 1.0f})
            .Execute([](RGPassContext& ctx) {
                Renderer3D::RecordScene(ctx.Cmd());
                Renderer3D::ClearPendingDraws();
            });
        m_ScenePassDeclared = true;
    }
}
```

**Ordering constraint — the trap:** the ImGui pass is declared by
`VulkanImGuiBackend.cpp:111` during `End`. The scene pass must be declared **before** that
point in the frame, or ImGui lands at slot 0 and the scene renders *after* the UI. Call
`DeclareScenePass()` from wherever layer `OnUpdate` runs — before the ImGui backend's
`End`.

**Step 4: Run — expect pass.**

**Step 5: Verify** (the real gate)

```bash
cmake --build build_debug -j8
timeout 25 ./bin/Debug-linux-x64/UHE_EDITOR > /tmp/e.log 2>&1
grep -cE "VUID|validation error|\[error\]" /tmp/e.log    # expect 0
```
Then **load a model and confirm it renders, with UI on top**. A scene that renders
underneath the UI is the proof the pass ordering is right.

**Step 6: Commit** (ask first)

---

### Task 4: Make graph failure loud instead of silent

**Why:** `m_FrameGraphFailed` currently drops the whole scene with a log line. This
session already produced three silent-failure bugs; do not add a fourth by omission.

**Files:**
- Modify: `UHE/src/Platform/Vulkan/VulkanDevice.cpp:579-625`
- Test: `tests/rendergraph/main.cpp`

**Step 1: Write the failing test**

```cpp
// A graph that fails to compile must report, not quietly produce an empty frame.
REQUIRE(Device().FrameGraphFailed());
REQUIRE(Device().FrameGraphErrorCount() > 0);
```

**Step 2: Run — expect failure** (no error accumulator exists).

**Step 3: Implement**

Accumulate the compile/resolve error messages into a member the frame loop can surface,
and emit one prominent log line naming the failing pass and the reason. Keep the legacy
fallback — removing it is out of scope — but make the fallback itself log loudly.

**Step 4: Run — expect pass.**

**Step 5: Verify**

Break a pass declaration deliberately, confirm the editor reports which pass and why
instead of showing an empty viewport.

**Step 6: Commit** (ask first)

---

### Task 5: Extend rendergraph tests to cover scene-shaped graphs

**Why:** the current 24 checks only ever validate an ImGui-only graph. That is the gap
that let "the graph is wired up" and "the graph carries your rendering" be confused.

**Files:**
- Modify: `tests/rendergraph/main.cpp`
- Modify: `tests/rendergraph/CMakeLists.txt` if a new fixture file is added

**Step 1: Write failing tests**

Cover:
- two-pass graph compiles and preserves declaration order
- depth attachment resolves
- undeclared attachment is reported by name (regression guard for the `Write()`-before-`Color()` trap documented at `VulkanDevice.cpp:551`)
- scene pass before ImGui pass keeps scene at slot 0
- a scene pass that reads the swapchain and writes an offscreen target compiles

**Step 2: Run — expect failures** for the uncovered cases.

**Step 3: Implement** whatever the tests show is missing; if they all pass, the tests
document existing behaviour (still worth having).

**Step 4: Run — expect pass.**

**Step 5: Verify**

```bash
cd build_debug && ctest -L unit --output-on-failure
```

**Step 6: Commit** (ask first)

---

## Files likely to change

| Path | Why |
|---|---|
| `UHE/src/UHE/Renderer3D/Renderer3D.h` | deferred-submit API, depth handle |
| `UHE/src/UHE/Renderer3D/Renderer3D.cpp` | `DeclareScenePass`, `RecordScene`, depth creation |
| `UHE/src/Platform/Vulkan/VulkanDevice.cpp` | expose frame graph / register scene resources |
| `UHE/src/Platform/Vulkan/UI/VulkanImGuiBackend.cpp` | pass declaration ordering |
| `UHE/src/UHE/Renderer/Framebuffer.{h,cpp}` | maybe a depth spec field |
| `tests/rendergraph/main.cpp` | new coverage |
| `tests/rendergraph/CMakeLists.txt` | if fixtures are added |

---

## Risks and tradeoffs

- **Silent scene loss is the dominant risk.** A compile error in the scene pass
  degrades to an empty viewport. Task 4 mitigates it; until then, verify visually after
  every change.
- **Pass ordering is easy to get backwards** and the failure mode is "UI renders, scene
  renders on top" or "scene is hidden". Both look like a content bug, not a graph bug.
- **All passes still share one primary command buffer.** `EndFrameGraph` serialises with
  `deps.push_back(previous)`; real parallelism needs per-pass command buffers (§8.5),
  explicitly deferred.
- **Push constants are full at 256 bytes.** Scene work needing more per-draw data must go
  into a descriptor.
- **Task 1 is the valuable checkpoint.** It is pure refactor with immediate submit still
  working, so it can be reverted independently if Stage 1 stalls.
- **Deferred submit changes `Renderer3D`'s threading contract.** Pass callbacks run on
  jobsystem tasks, so anything `RecordScene` touches must be safe from that thread. The
  existing camera/lights state is read during the scene walk on the main thread — the
  plan reads those into the `PendingDraw` at submit time rather than dereferencing
  `s_Data3D` later, but this deserves a deliberate decision, not an accident.

---

## Open questions for raj

1. **Stage 1 resolve target** — straight to swapchain (recommended) or offscreen from the
   start? Recommendation: swapchain. Nothing needs post yet.
2. **Should deferred submit be the default** once the scene pass lands, or should
   immediate remain default with graph opt-in? Recommendation: default to graph when the
   scene pass is present, keep immediate as a fallback flag.
3. **Task 4 ordering** — fail-loud before or after the first working scene pass? The
   risk argues for before, but the visible progress argues for after. Raj's call.
4. **Is the ImGui pass's swapchain write a conflict?** The scene pass writing the same
   swapchain image in an earlier slot is legal (sequential slots, barrier between), but
   worth confirming intent before coding.

---

## Not in this plan

- Offscreen HDR / post-processing (Stage 2, deferred)
- MSAA — no scene usage exists today; introducing it is a separate decision
- Per-pass command buffers / real graph parallelism (§8.5)
- Transparent / alpha-BLEND sorted pass — still needs this work done first
- Asset-side work (Track 2 registry, Track 1 material remainder) — separate branch
