#pragma once
#include "UHE/Core/Core.h"
#include "UHE/Core/Timestep.h"

#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace UHE
{
class Scene;

/**
 * \brief The Lua host: one VM, a sandboxed API, and the lifecycle scripts run in.
 *
 * Placement follows docs/architecture/scripting.md: scripting consumes the
 * engine, it is never a peer of the RHI, and the VM lives on the main thread
 * because a Lua state is not thread-safe. Nothing here needs a window, a device
 * or ImGui, so a headless run -- a training server stepping a scene -- hosts
 * scripts exactly the way the editor does.
 *
 * A script is attached to an entity with ScriptComponent, and the lifecycle is
 * the familiar one:
 *
 * ```lua
 * function OnCreate(self)      self.speed = 2.0          end
 * function OnUpdate(self, dt)  self:Translate(0, 0, self.speed * dt) end
 * function OnDestroy(self)                               end
 * ```
 *
 * `self` is a table, so an instance keeps its own fields, and it carries the
 * entity's methods (`GetPosition`, `SetLinearVelocity`, `Destroy`, ...). Every
 * call is protected: a script error is logged with its chunk name and traceback,
 * disables that instance, and never reaches the frame.
 *
 * The sandbox opens `base`, `math`, `string`, `table`, `utf8` and `coroutine`;
 * `io`, `os`, `package` and `debug` are not reachable, and `load`, `loadfile` and
 * `dofile` are removed, so a script cannot read the disk or build code at runtime.
 * Shared globals and library tables are read-only, so one script cannot break
 * another, and every call runs under an instruction budget (SetInstructionBudget).
 */
class UHE_API ScriptEngine
{
public:
    static ScriptEngine& Get();

    /// Opens the VM, the sandboxed libraries and the engine API. Idempotent.
    void Init();
    void Shutdown();
    [[nodiscard]] bool IsInitialized() const;

    /// The VM, for a test or a tool. A `lua_State*` rather than a sol2 type: sol2
    /// is a large template library and is deliberately kept out of the engine's
    /// headers.
    [[nodiscard]] lua_State* State() const;

    /// Compiles and runs a chunk. Errors are logged, not thrown.
    bool Run(const std::string& source, const std::string& chunkName);
    /// Reads and runs a script file. Errors are logged, not thrown.
    bool RunFile(const std::string& path);

    // ── Scene lifecycle ──────────────────────────────────────────────────────

    /// Instantiates every `ScriptComponent` in \p scene and calls `OnCreate`.
    void OnSceneStart(Scene& scene);

    /// Calls `OnUpdate` on every live instance, and starts the ones that appeared
    /// since the last update. Call once per simulation step.
    void Update(Scene& scene, Timestep ts);

    /// Calls `OnDestroy` on every instance belonging to \p scene and forgets it.
    void OnSceneStop(Scene& scene);

    /// Calls `OnDestroy` for one entity's instance, before its handle dies.
    ///
    /// \param entityId an `entt::entity` as an integer, so this header does not
    ///        have to include entt.
    void OnEntityDestroyed(Scene& scene, u32 entityId);

    // ── Hot reload ───────────────────────────────────────────────────────────

    /// Watches the source file of every instance and re-runs `OnCreate` when one
    /// changes. Off by default: a shipped build ships no script sources.
    void SetHotReloadEnabled(bool enabled);
    [[nodiscard]] bool IsHotReloadEnabled() const;

    /// Polls the watched sources, throttled; call once per frame.
    void Tick(Scene& scene);

    /// Recompiles and restarts every instance of \p scene, changed or not.
    void ReloadAll(Scene& scene);

    /// Recompiles and restarts one entity's instance.
    /// \returns false when the entity has no script.
    bool ReloadEntity(Scene& scene, u32 entityId);

    /// What one attached script is doing, for the editor's Scripts panel.
    struct InstanceInfo
    {
        u32 EntityId = 0;      ///< entt handle as an integer, for selection
        std::string EntityName;
        std::string Path;
        bool Started = false;
        bool Errored = false;
        u32 ReloadCount = 0;
        std::string Error;     ///< the last failure, empty while healthy
    };

    /// \returns one entry per instance belonging to \p scene, in entity order.
    [[nodiscard]] std::vector<InstanceInfo> Instances(Scene& scene) const;

    // ── Diagnostics ──────────────────────────────────────────────────────────

    /// Instances the engine is tracking (running, errored or not yet started).
    [[nodiscard]] u32 InstanceCount() const;
    /// Instances a script error has disabled. They stay disabled until the
    /// component's path changes or a reload is asked for.
    [[nodiscard]] u32 ErroredInstanceCount() const;
    /// The last error a script produced, with its chunk name and traceback.
    [[nodiscard]] const std::string& LastError() const;

    // ── Limits ───────────────────────────────────────────────────────────────

    /// @brief Default for SetInstructionBudget: far above any real per-frame
    /// script, low enough that an endless loop stops in well under a second.
    static constexpr u64 kDefaultInstructionBudget = 50'000'000;

    /**
     * @brief Caps the VM instructions one call into a script may run.
     *
     * A hook (or a nested chain of them, such as OnDestroy running inside
     * OnUpdate) that goes over is stopped with a script error, which disables
     * that instance like any other error, so `while true do end` cannot hang
     * the editor. Counted in steps of 10'000 instructions; time spent inside
     * engine bindings is not counted.
     *
     * @param instructions the limit; 0 removes it.
     */
    void SetInstructionBudget(u64 instructions);
    [[nodiscard]] u64 GetInstructionBudget() const;

    /// The implementation. Public so the file-local helpers in the translation
    /// unit can take it; the members themselves stay private.
    struct Impl;

private:
    ScriptEngine();
    ~ScriptEngine();
    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    std::unique_ptr<Impl> m_Impl;
};
} // namespace UHE
