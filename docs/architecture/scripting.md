# Scripting

Status: `DESIGN`. Issue [#28](https://github.com/unhuman-engine/UNHUMAN/issues/28).
Roadmap M6.3, decision D18. Code lands in `UHE/src/UHE/Scripting/` (new module)
and `ScriptComponent` in `Scene/Components.h`. Written against `improve_vulkan`
at `c271467`.

Issue #28 only states that Lua is the preferred choice. This document supplies
the missing design: what is embedded, where it sits, how scripts attach to
entities, how they run and fail, how the API is bound, and a phased plan.

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

Phase 1, the core: vendor Lua and sol2, implement `Init` and `Shutdown` with the
sandbox and panic handler, bind math, entity transform, input, time and logging,
add `ScriptComponent` and `ScriptSystem` with `OnCreate`, `OnUpdate` and
`OnDestroy`, and route calls through protected functions with error logging. Done
when a script moves a cube, reacts to a key, and an error logs instead of
crashing.

Phase 2, ergonomics and safety: reflection-driven component access, coroutines
with a frame budget and watchdog, hot reload by re-running `OnCreate`, and editor
console error reporting.

Phase 3, editor and events: script inspector, play and stop lifecycle wired to
scene start and stop, event hooks, entity create and destroy from script, API
annotation export, and debugger attach.

Phase 4, shipping: bytecode caching in the cook step, the module becoming
optional for non-script builds, and profiler integration.

## Decisions

| Item | Proposal |
|---|---|
| Language | Lua 5.4 with sol2, settled |
| VM count | One VM until isolation is needed |
| Binding style | sol2 runtime bindings, reflection registry in phase 2 |
| Execution | Coroutines with a frame budget; plain calls in phase 1 |
| Sandbox | No os, io, debug or package; engine API only |
| Storage | Assets by ID, source in development and bytecode when shipped |
| Threading | Main thread only |
| Shipping form | Optional module, compiled out for non-script builds |

## Pitfalls

Do not let a Lua state be touched from two threads. Do not expose pointers to GPU
objects or the registry. sol2 usertypes hold references, so bind to stable engine
objects rather than temporaries. Keep the API small and stable, since every
exposed function is a long-lived contract. Avoid per-entity Lua evaluation in hot
loops for large counts; batch or use events. Catch every call, including in
destructor paths. Include chunk names and debug information in error logs, or
debugging becomes guesswork.
