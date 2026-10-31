# Scripting

Status: `IMPLEMENTED` for phase 1 plus hot reload (issue
[#28](https://github.com/unhuman-engine/UNHUMAN/issues/28), roadmap M6.3,
decision D18). Code is in `UHE/src/UHE/Scripting/`, `ScriptComponent` in
`Scene/Components.h`, and the harness in `tests/scripting/`.

Issue #28 only states that Lua is the preferred choice. This document supplies
the design: what is embedded, where it sits, how scripts attach to entities, how
they run and fail, how the API is bound, and a phased plan. The **Implemented**
section below says which parts of it are in the tree and where the code deviates.

## Implemented

What a script is given, and what happens around it:

| Piece | Where | Notes |
|---|---|---|
| The VM, sandbox and bindings | `Scripting/ScriptEngine.{h,cpp}` | One `sol::state`, main thread, opened by `Application` |
| Attachment | `ScriptComponent` in `Scene/Components.h` | `ScriptPath` (project-relative), `Enabled`, runtime `RuntimeIndex` |
| Lifecycle | `OnCreate(self)`, `OnUpdate(self, dt)`, `OnDestroy(self)` | Driven from `Scene::OnUpdateEditorSimulation` / `OnUpdateRuntimeSimulation` |
| `self` | a table per instance | Keeps the script's own fields; carries the entity methods through a shared metatable |
| Entity API | `GetName`/`SetName`, `GetPosition`/`SetPosition`/`Translate`, `GetRotation`/`SetRotation`, `GetScale`/`SetScale`, `GetID`, `IsValid`, `Destroy`, `Spawn`, `Find` | Every call re-resolves the entity through the registry, so a destroyed entity answers "invalid" instead of reading freed memory |
| Rigid bodies | `HasRigidBody`, `Get`/`SetLinearVelocity`, `Get`/`SetAngularVelocity`, `ApplyForce` | What makes a script a controller rather than a cutscene |
| Engine API | `Log.{info,warn,error}`, `Input.IsKeyPressed`, `Input.IsMouseButtonPressed`, `Input.GetMousePosition` | Input checks for a window first, so a headless run answers `false` instead of asking GLFW about a null context |
| Hot reload | `ScriptEngine::Tick` | Source mtimes polled four times a second from the scene step; an edit recompiles and re-runs `OnCreate`. The editor and the sandbox enable it, **File > Reload Scripts** forces it |
| Errors | every call is a protected call | Logged with the chunk name and a traceback, the instance is disabled, the frame continues |

Deviations from the design below, all deliberate:

- **`sol::state& Lua()` is not in the public header.** sol2 is a large template
  library; `ScriptEngine.h` exposes `lua_State* State()` instead, and only
  `ScriptEngine.cpp` includes sol2.
- **`ScriptComponent` holds a path, not `AssetHandle<ScriptAsset>`.** The asset
  manager is not wired into the engine yet (nothing loads through it), so a
  handle would be a promise the tree cannot keep. The path resolves through
  `ResolveAssetPath`, the same seam textures and models use.
- **A Lua panic aborts** rather than "stopping the script". A panic means the VM
  was used outside a protected call, which is a bug in the host: Lua unwinds by
  `longjmp`, and returning from the handler would unwind through C++ frames.
  Every script call is protected, so this should be unreachable.
- **No coroutines, frame budget or instruction watchdog yet**, and no
  reflection-driven component access: those are phase 2 items below.
- **`GetID` returns a string.** An entity id is a random 64-bit UUID and a Lua
  integer is signed, so half of them cannot be represented as a number.

A working example ships with the editor's assets
(`UHE_EDITOR/assets/scripts/orbit.lua`): attach it to an entity with the inspector
and it orbits the origin, logging where it is.

What the harness proves (`tests/scripting`, 53 checks, no GPU): the sandbox, the
lifecycle order, `self` state across frames, the entity API, a controller loop
that converges on a target, an error disabling one instance while the next keeps
running, a missing file, hot reload, `Enabled = false` pausing, and the script
surviving a scene save/load round trip.

## Decision

Lua 5.4 with sol2. It is small, fast, embeddable, the de-facto gameplay
scripting language, easy to hot-reload, and sol2 is header-only with no codegen
step. This is settled; no other language is under consideration.

Because Lua has no static types, correctness comes from tooling: LuaLS or
EmmyLua annotations generated from the binding table, plus luacheck in CI.

Non-goals: no native FFI, no user-defined C++ types, and no reflection exposing
the whole engine. The script API is a curated, versioned surface.

## Placement

Scripting is a consumer of the engine, not a peer of the RHI. It must never
include Vulkan or RHI headers, and the VM runs on the main thread, because a Lua
state is not thread-safe.

Scripts are assets (`.lua`) loaded through the asset system.

## VM ownership

One `sol::state` owned by a `ScriptEngine`.

```cpp
class ScriptEngine {
public:
    void Init();                        // open libs, sandbox, set panic handler
    void Shutdown();
    sol::state& Lua();

    void BindCore();                    // math, time, input, log
    void BindScene(Scene&);             // entity and component API

    bool Run(const std::string& source, const std::string& chunkName);

    void OnSceneStart(Scene&);
    void Update(Timestep);              // drives ScriptSystem with a time budget
    void OnSceneStop(Scene&);
};
```

Start with a single VM. Per-scene VMs are only worth it later, for isolation. Set
`lua_atpanic` so an unrecoverable Lua error logs and stops the script rather than
the process.

## Entity integration

```cpp
struct ScriptComponent {
    AssetHandle<ScriptAsset> Script;   // .lua asset
    bool Started = false;
    bool Enabled = true;
    u32 RuntimeIndex = kInvalidIndex;  // per-instance state owned by ScriptSystem
};
```

Lifecycle functions follow the familiar MonoBehaviour shape:

```lua
function OnCreate(self)             end
function OnUpdate(self, dt)         end
function OnDestroy(self)            end
function OnEvent(self, name, data)  end
```

`self` is a table bound to the entity handle, so instances hold their own fields
(`self.health = 100`). Multiple scripts per entity can be supported later, either
by attaching to child entities or by storing a list of script handles. Start with
one script per component.

The system tick iterates entities with a `ScriptComponent`, calls `OnCreate`
once, then resumes `OnUpdate` while the component is enabled.

## Execution model

Plain calls are acceptable for the first phase. The target is a coroutine per
instance so scripts can yield to wait for a frame or a duration without threads
and without stalling the frame. Each instance stores a `sol::coroutine`, and the
system resumes it each frame.

A frame budget in `ScriptEngine::Update` tracks elapsed script time; if a script
exceeds a threshold such as 4 ms, the remaining instances are deferred to the
next frame and a warning is logged. A `lua_sethook` instruction-count watchdog
covers a genuine infinite loop.

## Binding

Bindings use sol2 `new_usertype` and `set_function`, in two tiers.

The first tier is hand-bound and covers math types, `Entity` (transform access,
destroy, validity), input, time, logging, audio playback, and scene operations
such as spawn and find.

The second tier is reflection-driven component access. Components are registered
once, for example `UHE_REGISTER_COMPONENT(TransformComponent, Translation,
Rotation, Scale)`, which allows `entity:get("TransformComponent")` from Lua. This
removes the N-by-M hand-binding cost and is the main reason to do the second
phase.

Binding rules: never expose raw GPU or RHI handles, always resolve components
through the registry rather than caching pointers because entities move, and
version the API so breaking changes are explicit.

## Errors, sandboxing, determinism

Every call goes through `sol::protected_function`. On failure, log the error with
a traceback, disable that instance, and surface it in the editor console. A
scripting error must never crash a frame.

The sandbox does not open `io`, `os`, `package` or `debug` (except possibly
`debug.traceback`), and removes `loadfile`, `dofile` and `load`. Only `math`,
`string`, `table`, `utf8` and `coroutine`, plus the engine API, are reachable.

For determinism, note that `pairs()` iteration order is not stable. Either
document that ordered iteration uses arrays, or provide a deterministic iterator.
Scripts should read time from the engine, not the wall clock.

## Hot reload

Watch `.lua` files through the asset system's file watcher. On change, recompile
the chunk and either re-run `OnCreate`, which is simple but loses state, or
preserve the `self` table and rebind `OnUpdate`. Start with the first approach,
and gate reload behind a toggle so shipping builds can disable it.

## Threading

Main-thread execution only. A script may dispatch a job, but must not touch the
VM from a worker; results are marshalled back and delivered on the next frame. If
parallel script evaluation is ever needed, use one VM per thread with no shared
mutable state, not a shared state with locks.

## Tooling

Generate LuaLS or EmmyLua annotations from the binding table so editors and
VSCode autocomplete the API. Attach the VSCode lua-debugger over a local socket
in phase three. Route script logs to the editor console. Run luacheck over the
script assets in CI.

## Build and assets

Vendor Lua 5.4 (C) and sol2 (header-only), or fetch them through vcpkg. Add a
`scripting` module depending on `scene`, `core` and `assets`, never on a
backend. Scripts are assets with stable IDs, so `ScriptComponent` stores a
handle rather than a path. The cook step can store precompiled bytecode in the
runtime bundle, with source used in development.

## Phases

Phase 1, the core: **DONE**. Lua and sol2 are vendored, `Init`/`Shutdown` open the
sandbox, the entity, input and logging bindings exist, `ScriptComponent` runs
`OnCreate`/`OnUpdate`/`OnDestroy`, and every call is protected with error
logging. A script moves an entity, reads a key, and an error logs instead of
crashing -- all covered by `tests/scripting`.

Phase 2, ergonomics and safety: **hot reload DONE** (mtime poll, recompile,
re-run `OnCreate`; script logs and errors reach the editor console through the
normal log sink). Still open: reflection-driven component access, coroutines with
a frame budget and an instruction watchdog.

Phase 3, editor and events: **script inspector DONE** (path, enabled toggle) and
entity create/destroy from script is DONE (`Spawn`, `Find`, `Destroy`). Still
open: event hooks (`OnEvent`), API annotation export, debugger attach.

Phase 4, shipping: bytecode caching in the cook step, the module becoming
optional for non-script builds, and profiler integration.

## Decisions

| Item | As implemented |
|---|---|
| Language | Lua 5.4.7 vendored in `UHE/vendor/lua`, sol2 vendored in `UHE/vendor/sol2` (single header, pinned commit: the 3.3.0 release does not compile under C++23) |
| VM count | One `sol::state`, owned by `ScriptEngine::Get()`, opened by `Application` |
| Binding style | sol2 runtime bindings; `sol` appears in `ScriptEngine.cpp` only |
| Execution | Plain protected calls; coroutines and the frame budget are still open |
| Sandbox | `base`, `math`, `string`, `table`, `utf8`, `coroutine`; `debug` is opened only to capture `debug.traceback` and then dropped. `load`, `loadfile`, `dofile`, `require` and `print` are removed |
| Storage | `ScriptComponent::ScriptPath`, project-relative, resolved by `ResolveAssetPath`; bytecode cooking is not done |
| Threading | Main thread only: the VM is driven from the scene step |
| Shipping form | Always built; hot reload is off unless an app turns it on |

## Pitfalls

Do not let a Lua state be touched from two threads. Do not expose pointers to GPU
objects or the registry. sol2 usertypes hold references, so bind to stable engine
objects rather than temporaries. Keep the API small and stable, since every
exposed function is a long-lived contract. Avoid per-entity Lua evaluation in hot
loops for large counts; batch or use events. Catch every call, including in
destructor paths. Include chunk names and debug information in error logs, or
debugging becomes guesswork.
