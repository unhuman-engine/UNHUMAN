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
- **A pipeline's vertex layout and push constant range come from the module.**
  `ShaderReflection` reads them out of the SPIR-V, so a shader edit that changes
  its interface fails `tests/shader` instead of drawing garbage. Never re-add a
  hand-written copy of what the shader says.
- **Destroy GPU resources only after a wait.** Uploads and draws are queued;
  destroying a texture or buffer a submission still reads is invalid. Teardown
  waits at frame end, or `WaitIdle()` for a one-off — including the pipeline
  rebuild a shader hot reload triggers.

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
- Sanitizer presets already exist: `cmake --preset sanitizer -DUHE_SANITIZER=<address|thread|undefined>`.
  Binaries land in `bin/Debug-linux-x64-<sanitizer>/`.
