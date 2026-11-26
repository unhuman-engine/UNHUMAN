# Working rules for this repository

Engine: C++20, Vulkan 1.3 via the RHI, Slang shaders, ImGui editor. These are the
rules that are *not* guessable from the code and that have each caused a real bug
here. Follow them; if a rule is wrong, change it here first.

## Rendering

- **A pipeline is created for the pass it is bound in.** Colour attachment
  *count* and *format* both have to match: `Renderer3D` builds the editor's scene
  pipelines for two attachments (`RGBA8_SRGB` + `R32_SINT` for picking) and the
  game path draws into one attachment in `device.GetSwapchainFormat()`. Those are
  separate pipeline variants, selected with `Renderer3D::SetSwapchainTarget`.
  Binding the editor's pipeline into the swapchain pass was 34 validation errors
  per frame.
- **A fragment shader must not write an output the pass has no attachment for.**
  The swapchain variant of `Basic3D.slang` is compiled with `UHE_NO_ENTITY_ID`
  so it does not write `SV_Target1`.
- **Attachments track the swapchain extent, not the window.** `passDesc.renderWidth/Height`
  and the viewport come from `swapChain.GetExtent()`. The window's own size can
  still be the pre-resize one, and a render area taller than an attachment is
  `VUID-VkRenderingInfo-pNext-06080`.
- **Recreate an attachment when the extent changes.** A depth/colour texture made
  once at attach time goes stale on the first resize. `WaitIdle()` before
  destroying the old one: earlier frames can still be reading it.
- **Shaders are Slang, one source per variant.** A define set is declared once in
  `UHE_EDITOR/assets/shaders/variants.txt` (`no_entity_id: UHE_NO_ENTITY_ID=1`) and
  asked for by name (`ShaderCache::Load(path, "no_entity_id")`), rather than
  keeping two near-identical files in step or repeating the define at each call
  site. Entry points are fixed: `vertexMain`, `fragmentMain`, `computeMain`.
- **The build compiles shaders, not the runtime.** `uhe_shaderc` (the
  `uhe_shaders` target) writes `<stem>.<stage>.<variant>.spv` next to each source;
  `ShaderCache::Load` reads that artifact instead of running Slang, which used to
  sit on the startup path of every launch. A runtime that compiles is either hot
  reload or a source missing from `UHE_SHADER_SOURCES` in `tools/CMakeLists.txt`.
- **A `.spv` is a cache, trusted only when complete and valid.** `ShaderCache`
  compiles the source instead when an artifact is older than its source, when
  a vertex stage has no fragment (or the reverse), or when a file is not valid
  SPIR-V. Loading what was there used to hand pipelines half a shader.
- **A pipeline's vertex layout and push constant range come from the module.**
  `ShaderReflection` reads them out of the SPIR-V, so a shader edit that changes
  its interface fails `tests/shader` instead of drawing garbage. Never re-add a
  hand-written copy of what the shader says.
- **ImGui colours are sRGB; the swapchain is sRGB too.** ImGui's own shader
  writes theme colours unchanged and the swapchain encodes them a second time,
  which drew the whole editor washed-out grey (a 0.095 background showed as
  0.34). `ImGuiSrgb.slang` replaces ImGui's shaders and decodes the vertex
  colour first; keep it in `UHE_SHADER_SOURCES`, and author theme and draw-list
  colours in sRGB as a colour picker shows them.
- **Every `GraphicsPipelineDesc` field is part of the pipeline cache key.**
  `VulkanPipelineState::Hash` dedupes pipelines by the fields it hashes; a new
  field it leaves out makes two different descs share one pipeline. The
  `wireframe` flag did exactly that: the wireframe view drew filled.
- **Destroy GPU resources only after a wait.** Uploads and draws are queued;
  destroying a texture or buffer a submission still reads is invalid. Teardown
  waits at frame end, or `WaitIdle()` for a one-off — including the pipeline
  rebuild a shader hot reload triggers.

- **New rendering declares FrameGraph passes.** `RHIDevice::GetFrameGraph()`,
  `AddPass<Data>(setup, execute)`: the pass names what it reads and writes and
  for what use, and the render graph places every barrier. A pass records with
  `FGContext::Cmd()` and has no Vulkan types; a texture it needs only inside
  the frame is `FGBuilder::Create`, from a pool, not a hand-made attachment
  that has to be resized and destroyed. Handles do not survive the frame.
- **A target read after the frame is exported, not imported.** The editor's
  viewport image and picking read its scene colour and entity IDs after the
  graph ran: those are `FGBuilder::Create` plus `FrameGraph::Export`, fetched
  each frame with `RHIDevice::GetExportedTexture(name)`. Do not keep the
  returned texture: a resize replaces it. `Import` is for what the graph does
  not own (the swapchain, a model's textures).
- **A frame graph error is a defect.** A graph that fails to translate,
  compile or resolve drops its passes and the frame still presents, so the
  only symptom is a missing scene. Under `UHE_VALIDATION_FATAL` it stops the
  run like a validation error does.
- **The compiled render graph is cached by exact structure, not by hash.**
  `VulkanRenderGraphCompileCache` replays the last compile only when every
  access, state and name matches, and rebinds handle generations. The topology
  hash counts a pass's reads instead of naming them; never key a cache on it.
  `UHE_RENDERGRAPH_NO_CACHE=1` compiles every frame, for an A/B.
- **Passes record one at a time, in compiled order.** They all record into
  the frame's one command buffer, and renderers like `Renderer3D` keep global
  state: two independent passes (shadow cascades and the depth prepass) once
  recorded at the same time from two job threads and corrupted the heap.
  `MapToTaskgraph` chains every pass after the previous one; parallel
  recording would need a command buffer per pass first.
- **The 3D scene is drawn by `SceneRenderer`, in several passes.** Shadow
  cascades, the depth prepass, the colour pass and water all draw the scene,
  and they must agree on where everything is: a prepass and a colour pass that
  disagree leave holes. `Scene::PrepareRenderFrame` builds the transforms once
  per frame, the camera is resolved once when the first pass executes, and
  the depth-only pipelines use the colour pass's own vertex shader.
- **Per-frame shading data lives in `Renderer3D::SceneFrameData`, not in push
  constants.** Basic3D's push constants are 208 of the 256 bytes a device must
  support; shadows, ambient, occlusion and wind are one storage buffer read
  through `frameDataIndex`. `Basic3D.slang` and `Water.slang` each declare its
  layout, so a field added to the C++ struct is added to both.
- **Every write is followed by a barrier.** Two passes rendering into one
  attachment in a row are separate rendering scopes; the compiler orders them
  even when the layout does not change, and attachment access includes the
  read that a load, blend or depth test does.

## Entities and scenes

- **An `Entity` kept across frames is checked, not trusted.** Play, Stop and
  opening a scene or a project replace the active scene; an `Entity` stored
  in a panel or a member (the hovered entity, a selection) then names a scene
  that is gone. `Entity` remembers its scene's instance id: `if (entity)`,
  `IsValid()`, `HasComponent` and `TryGetComponent` answer false or null for
  it. `GetComponent` does not check: use it only right after a check, or use
  `TryGetComponent`. Never keep a raw `Scene*` or `entt::entity` instead.

## Cooked assets

- **A game loads cooked models, not glTF.** Exports cook every referenced
  model to `<model>.uhemodel` (`Model::SaveCooked`, through
  `ExportOptions::CookModel`) and drop the glTF with the files only it used;
  `Model::loadModel` takes the cook when the source is missing or older.
  `uhe_cook --inspect` shows what a cook holds.
- **A model field is a cooked-format field.** `CookedModel.cpp` writes every
  field of `Material`, `MaterialExtensions`, `Primitive`, `Mesh`, `ModelNode`,
  the lights, skins and clips by name. A field added to one of them is added
  there too, and `kModelVersion` goes up so an older cook is recooked instead
  of misread; `tests/gltf` compares a cooked load with the glTF load.
- **Cooking never touches the GPU.** It loads with `upload = false` and
  `keepTextureSources = true`; `uhe_export` links stubs that abort on any
  texture or buffer creation, so a change that makes cooking upload fails there.

## Physics for robots

- **Joints are built from the poses at Play start.** `Physics::CreateJoints`
  runs after the bodies exist; `Joint3DComponent::Anchor`/`Axis` are in the
  entity's space and encoders read from that starting pose. A joint is torn
  down before either body (`DestroyJointsOf` in `DestroyEntity`,
  `DestroyAllJoints` in `OnRuntimeStop`): a constraint must not outlive a body.
- **Sensors read the last simulation tick, never the render cache.** Encoders
  and IMUs update in `Physics::UpdateSensors` after each step; a script ray
  walks the transform tree now (`GetWorldSpaceTransformMatrix`) because the
  render cache is a frame behind. That keeps a run reproducible tick for tick,
  which `tests/scene` checks bit-for-bit.

## Scripting is a consumer of the engine

- **The VM is main-thread only and never touched from a job.** A Lua state is not
  thread-safe; scripts are driven from the scene step (`Scene::OnUpdate*Simulation`
  → `ScriptEngine::Update`). A script that wants concurrency dispatches a job and
  reads the result on a later frame.
- **Every script call is protected.** A failing `OnCreate`/`OnUpdate`/`OnDestroy`
  is logged with its chunk name and traceback, disables that instance, and lets the
  frame continue. Never call a hook unprotected, and never let one reach the frame.
- **The sandbox is the API.** `io`, `os`, `package` and `debug` are not reachable,
  `load`/`loadfile`/`dofile` are removed, and scripts live in the project's assets
  where `ResolveAssetPath` finds them. Adding a binding means adding to that
  surface deliberately: every exposed function is a long-lived contract.
  Shared globals and library tables are read-only proxies (`_G` is the script's
  own environment), and every call runs under an instruction budget, so an
  endless loop disables its instance instead of hanging the frame.
  A proxy must never hand out the table behind it: its `__pairs` iterates
  through a closure, because `next, t, nil` gave `local _, real = pairs(string)`
  write access to the library every script shares.
- **A script resolves its entity through the registry on every call.** `self` is a
  table holding an entity token, not a pointer: a script can outlive the entity it
  was attached to, and `IsValid()` must answer for that case instead of crashing.
- **A script error must not be a build or startup failure.** `tests/scripting`
  covers the lifecycle, the API, errors and hot reload with no GPU, because all of
  it is logic.

## Validation is a gate, not a log

- Run `script/check_validation.sh` before pushing. It runs the engine with
  `UHE_VALIDATION_FATAL=1`, which stops the process on the first validation
  *error* and fails the run.
- **A gate that cannot see an error must not report a clean run.** The script
  and CI require the `[VALIDATION] enabled` line the engine logs at startup, so
  a build without validation layers — `enableValidationLayers` is off under
  `NDEBUG` — fails instead of passing vacuously.
- `UHE_VALIDATION_FATAL` follows the usual on/off spelling: unset, empty, `0`,
  `false`, `off` and `no` are off. `UHE_VALIDATION_FATAL=0` means off; do not
  read "set" as "on".
- CI applies the same gate in `.github/workflows/sanitizers.yml` (Sandbox smoke,
  ASan + TSan) and `test-renderer.yml` (editor). A run that renders 34 broken
  frames a second and survives is a failure, not a pass.
- **Run synchronization validation after touching barriers.** `-sync-validation`
  or `UHE_SYNC_VALIDATION=1` turns on the layer's hazard tracking, which the
  ordinary gate does not do. It found two real races: depth barriers named only
  the late fragment tests although a load or clear runs at the early ones
  (`Stage::DepthTests` covers both), and a resource's first barrier in a frame
  waited on `TopOfPipe`, which orders nothing before it (the previous frame's
  writes to a pooled image, the swapchain acquire). It is now `AllCommands` +
  `MemoryWrite`.
- Severity is mapped honestly in `VulkanInstance::debugCallback`: errors are
  errors, warnings are warnings. The layer's "writes to Location 1 ... and this
  write is unused" is a warning. A message the layer repeats is logged in full
  once and then only at 2, 4, 8 ... times, so the first occurrence is not buried
  by the thousandth.
- That policy is `UHE/src/Platform/Vulkan/ValidationGate.*`, deliberately free
  of Vulkan headers so `tests/validation` can exercise it without a device. The
  severity bits are mirrored there; `VulkanInstance.cpp` static_asserts the
  mirror still matches the layer's.

## Architecture

- **Nothing above `UHE/src/Platform/Vulkan` includes Vulkan headers.** Engine
  code goes through `RHI::RHIDevice`; a new device capability is a virtual on
  that interface plus the backend implementation (see `GetSwapchainFormat`).
  Demo layers may reach the swapchain for its extent — that is the documented
  exception.
- **No identifier is called `near` or `far`.** `windows.h` defines both as
  macros: a variable with either name compiled everywhere and broke the
  Windows build, twice. `nearPlane`, `farPlane`, `closeTo`.
- **Editor code reads ImGui internals through `ImGui::GetCurrentContext()`.**
  The inline helpers in `imgui_internal.h` (`GetCurrentWindow`, `GetActiveID`,
  ...) read the global `GImGui`, which the engine DLL does not export on
  Windows: the editor compiled everywhere and failed to link on Windows only.
- **Wait on children with a continuation, not a nested `Wait`.** `Wait()` runs
  other jobs on the waiting thread's stack, so waits inside jobs grow it;
  `UheJobsystem::Then(counter, job)` or a `JobTask` coroutine's
  `co_await jobs.WhenDone(counter)` resume with no thread held. A
  `JobCounter` may be destroyed only after `Wait()` on it returned or from its
  continuation: the job that finished the group is still inside it until then.
  The deque is Chase–Lev with seq_cst operations instead of fences, because
  TSan (which CI runs on `tests/jobsystem`) does not model fences.
- **Behaviour lives in `UHE/src/UHE`; apps compose layers.** `sandbox`,
  `UHEGAME` and `UHE_EDITOR` are composition roots, not engines.
- **The engine and the editor share one scene format**: `.unhuman` YAML, with
  `MeshRendererComponent`/`MeshPath` for meshes. Asset paths resolve through the
  asset registry after `mount_project_roots`.

## Tests and CI

- CPU-side unit tests are standalone programs under `tests/`, run by
  `ctest -L unit`. Add one when a change is pure logic. CI runs them in
  `build-linux.yml` (Release) and under ASan and TSan in `sanitizers.yml`:
  building a harness is not running it, and a harness that stopped linking
  once went unnoticed for that reason.
- **The `.spv` artifacts are shared by every build directory.** They sit next
  to the shader sources, and Slang's output differs slightly between an ASan or
  TSan build and a plain one. After building a different configuration, the
  `shader` test compares against the other build's artifacts and fails its
  byte-identical checks; run that configuration's `uhe_shaderc --force` first.
  CI builds one fresh tree per job, so it is not affected.
- Anything that touches the device is covered by the Sandbox smoke instead: it
  logs `[SMOKE-READY] first frame rendered` once frame 1 has been submitted and
  presented, and CI waits for that line rather than a wall clock.
- **The golden-image step checks what is rendered, not only that it rendered.**
  `test-renderer.yml` runs the Sandbox on lavapipe with `UHE_FIXED_TIMESTEP` and
  `UHE_CAPTURE_FRAME` (`UHE/src/UHE/Core/CaptureSettings.h`), and `uhe_imgdiff`
  compares frame 30 with `tests/golden/sandbox_box.png`. An intended visual
  change replaces that file with the run's `actual.png` artifact, never with a
  frame from a local GPU: see `tests/golden/README.md`.
- **Lint gates the lines a PR changes.** `lint.yml` fails on misformatted
  changed lines and on `bugprone-*` findings in them. Both tools are pinned
  (`pip install clang-format==22.1.8 clang-tidy==22.1.8`), because formatting
  differs between releases; use the same versions locally. Formatting can
  reorder includes: a header that is not self-contained (`ImGuizmo.h` needs
  `imgui.h` first) keeps its order inside `// clang-format off`.
- Sanitizer presets already exist: `cmake --preset sanitizer -DUHE_SANITIZER=<address|thread|undefined>`.
  Binaries land in `bin/Debug-linux-x64-<sanitizer>/`.
