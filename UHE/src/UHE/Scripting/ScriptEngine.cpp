#include "uhepch.h"
#include "ScriptEngine.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Core/Log.h"
#include "UHE/Core/input.h"
#include "UHE/Physics/PhysicsSystem3D.h"
#include "UHE/Renderer3D/AnimationStateMachine.h"
#include "UHE/Scene/Components.h"
#include "UHE/Scene/Entity.h"
#include "UHE/Scene/Scene.h"

// sol2 is a large template library: it is included here and nowhere else, so no
// engine header and no other translation unit pays for it.
#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// The rigid-body bindings go through Jolt's body interface, which the physics
// system already owns.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyInterface.h>
// clang-format on

#include <GLFW/glfw3.h>
#include <chrono>
#include <cmath>
#include <fstream>
#include <system_error>

namespace UHE
{
namespace
{
/// The vector a script sees. Not `glm::vec3`: sol2 detects an ADL `to_string`
/// for a usertype's arguments and instantiates it, and glm's own `to_string`
/// does not compile when it is handed a sol2 type.
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    [[nodiscard]] float Length() const { return std::sqrt(x * x + y * y + z * z); }
};

Vec3 ToVec3(const glm::vec3& value)
{
    return {value.x, value.y, value.z};
}
} // namespace

struct ScriptEngine::Impl
{
    /// One entity's script: the compiled hooks, the instance table they run with,
    /// and where the source came from.
    struct Instance
    {
        Scene* Scene_ = nullptr;
        entt::entity Entity = entt::null;
        std::string Path;         // as the component spells it
        std::string ResolvedPath; // what was actually read
        std::filesystem::file_time_type SourceTime{};
        sol::table Self;
        sol::protected_function OnCreate;
        sol::protected_function OnUpdate;
        sol::protected_function OnDestroy;
        bool Started = false;
        bool Errored = false;
        /// Released while a hook was running: stopped, invisible to every query,
        /// and erased once the outermost hook returns.
        bool Dead = false;
        std::string Error; // why this instance errored, for the editor
        u32 ReloadCount = 0;
    };

    sol::state Lua;
    bool Initialized = false;
    bool HotReload = false;
    double LastPollSeconds = -1.0;
    std::string LastError;
    u32 NextInstanceId = 1;
    std::unordered_map<u32, Instance> Instances;
    /// How many script calls are on the C++ stack. An instance cannot be erased
    /// while this is non-zero: the running hook may be its own (a script that
    /// destroys its entity), and sol reads the protected_function after the call.
    u32 HookDepth = 0;
    std::vector<u32> PendingRelease;
    sol::table EntityMethods;
    sol::function Traceback;
    /// @brief Read-only view of the globals every script environment falls back to.
    sol::table SharedGlobals;

    /// @brief VM instructions one outermost script call may run; 0 disables the limit.
    u64 InstructionBudget = ScriptEngine::kDefaultInstructionBudget;
    /// @brief Instructions counted since the outermost call began.
    u64 InstructionsUsed = 0;
    /// The scene the running instance belongs to: `Spawn` and `Find` are called
    /// from a script, and a script only exists inside a scene.
    Scene* ActiveScene = nullptr;
};

namespace
{
constexpr u32 kInvalidInstance = 0xFFFFFFFFu;

/// Jolt's "no body" id, as RigidBody3DComponent stores it.
constexpr u32 kInvalidBodyId = 0xFFFFFFFFu;

/// How often `Tick` looks at the watched sources.
constexpr double kHotReloadPollSeconds = 0.25;

/// @brief How many VM instructions run between two budget checks.
constexpr int kBudgetCheckInterval = 10'000;

/// @brief The Impl that owns a VM, kept in the state's extra space so the
/// count hook can find it. Coroutines copy the main thread's extra space.
ScriptEngine::Impl*& OwnerOf(lua_State* state)
{
    return *reinterpret_cast<ScriptEngine::Impl**>(lua_getextraspace(state));
}

/**
 * @brief Count hook: raises a Lua error once a call has used its budget.
 *
 * The error unwinds like any other script error, so the protected call that
 * started the script reports it and the instance is disabled instead of the
 * frame hanging on an endless loop.
 */
void BudgetHook(lua_State* state, lua_Debug* /*ar*/)
{
    ScriptEngine::Impl* impl = OwnerOf(state);
    if (impl == nullptr || impl->InstructionBudget == 0)
    {
        return;
    }
    impl->InstructionsUsed += kBudgetCheckInterval;
    if (impl->InstructionsUsed > impl->InstructionBudget)
    {
        // lua_pushfstring has no 64-bit format, and luaL_error longjmps, so no
        // object with a destructor may live in this frame: a plain buffer.
        char limit[32];
        std::snprintf(limit, sizeof(limit), "%llu", static_cast<unsigned long long>(impl->InstructionBudget));
        luaL_error(state, "script exceeded its instruction budget (%s instructions)", limit);
    }
}

/**
 * @brief Read-only shared globals for every script environment.
 *
 * Plain library tables (`math`, `string`, `Log`, ...) are replaced by read-only
 * proxies, the string metatable is hidden and `rawset` is removed, so no
 * script can change what another script sees.
 */
constexpr const char* kSealGlobals = R"lua(
local next, type, tostring, error, setmetatable, getmetatable = next, type, tostring, error, setmetatable, getmetatable
local function readonly(t, name)
    return setmetatable({}, {
        __index = t,
        __newindex = function(_, k) error("'" .. name .. "' is read-only (cannot set '" .. tostring(k) .. "')", 2) end,
        __pairs = function() return next, t, nil end,
        __len = function() return #t end,
        __metatable = false,
    })
end
local g = _G
for k, v in next, g do
    if type(v) == "table" and k ~= "_G" and getmetatable(v) == nil then
        g[k] = readonly(v, k)
    end
end
getmetatable("").__metatable = false
g.rawset = nil
return readonly(g, "_G")
)lua";

std::filesystem::file_time_type ModificationTime(const std::string& path)
{
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : time;
}

bool ReadFile(const std::string& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

/// Resolves a component's script path through the asset registry, so a script
/// lives with the project's other assets ("Scripts/drone.lua").
std::string ResolveScriptPath(const std::string& path)
{
    return ResolveAssetPath(path).string();
}

/// \returns the scene an entity table was made for, or nullptr for anything else.
///
/// The scene is carried as an opaque integer token rather than a userdata: a
/// script can neither read nor forge it, because every lookup additionally
/// requires the token to be the scene currently being driven.
Scene* SceneOf(const sol::table& self)
{
    const sol::object scene = self.raw_get<sol::object>("__scene");
    if (!scene.is<lua_Integer>())
    {
        return nullptr;
    }
    return reinterpret_cast<Scene*>(static_cast<std::uintptr_t>(scene.as<lua_Integer>()));
}

/// \returns the handle stored in an entity table, or entt::null.
entt::entity EntityOf(const sol::table& self)
{
    const sol::object id = self.raw_get<sol::object>("__entity");
    if (!id.is<lua_Integer>())
    {
        return entt::null;
    }
    return static_cast<entt::entity>(id.as<lua_Integer>());
}

/// Resolves and validates an entity table.
///
/// Entities are resolved through the registry on every call rather than cached:
/// a script can hold a table for an entity that has since been destroyed, and
/// that has to answer "invalid", not read freed memory.
bool ResolveEntity(ScriptEngine::Impl& impl, const sol::table& self, Scene*& scene, entt::entity& handle)
{
    scene = SceneOf(self);
    handle = EntityOf(self);
    return scene != nullptr && scene == impl.ActiveScene && handle != entt::null && scene->GetRegistry().valid(handle);
}

/// True when there is a window whose keys and buttons can be read.
///
/// Input is read from GLFW, which needs a current context: a headless run (a
/// training server) has none, and asking GLFW about a null window is a crash.
bool HasWindow()
{
    return glfwGetCurrentContext() != nullptr;
}

std::string DescribeEntity(Scene& scene, entt::entity handle)
{
    // A hook may have destroyed its own entity by the time its error is reported.
    if (!scene.GetRegistry().valid(handle))
    {
        return "<destroyed entity>";
    }
    Entity entity{handle, &scene};
    if (entity.HasComponent<TagComponent>())
    {
        return entity.GetComponent<TagComponent>().Tag;
    }
    return "<entity>";
}

/// The body interface for an entity, or nullptr when it has no rigid body or the
/// physics system is not initialized.
JPH::BodyInterface* BodyInterfaceOf(Scene& scene, entt::entity handle, JPH::BodyID& bodyId)
{
    Entity entity{handle, &scene};
    if (!entity.HasComponent<RigidBody3DComponent>())
    {
        return nullptr;
    }

    const auto& body = entity.GetComponent<RigidBody3DComponent>();
    if (body.RuntimeBodyID == kInvalidBodyId)
    {
        return nullptr;
    }

    JPH::BodyInterface* interface = scene.GetPhysicsSystem3D().GetBodyInterface();
    if (interface == nullptr)
    {
        return nullptr;
    }

    bodyId = JPH::BodyID(body.RuntimeBodyID);
    return interface;
}

glm::vec3 ToGlm(const JPH::Vec3& value)
{
    return {value.GetX(), value.GetY(), value.GetZ()};
}

JPH::Vec3 ToJolt(const glm::vec3& value)
{
    return {value.x, value.y, value.z};
}
} // namespace

namespace
{
/// Builds the table a script sees as `self`: an entity plus whatever fields the
/// script puts on it. The entity itself is stored raw, so the methods table
/// (shared by every instance) can find it.
sol::table MakeEntityTable(ScriptEngine::Impl& impl, Scene& scene, entt::entity handle)
{
    sol::table self = impl.Lua.create_table();
    self.raw_set("__scene", static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(&scene)));
    self.raw_set("__entity", static_cast<lua_Integer>(static_cast<u32>(handle)));

    sol::table metatable = impl.Lua.create_table();
    metatable[sol::meta_function::index] = impl.EntityMethods;
    // Hidden: the methods table is shared, and a script must not reach it to
    // replace `Destroy` for every other instance.
    metatable["__metatable"] = false;
    self[sol::metatable_key] = metatable;
    return self;
}

sol::protected_function HookOf(ScriptEngine::Impl& impl, const sol::environment& env, const char* name)
{
    const sol::object hook = env[name];
    if (!hook.is<sol::function>())
    {
        return {};
    }

    sol::protected_function function = hook.as<sol::protected_function>();
    if (impl.Traceback.valid())
    {
        function.set_error_handler(impl.Traceback);
    }
    return function;
}

/// Reports a failed protected call, with the chunk name and a traceback.
/// \returns the message it logged, or an empty string when the call succeeded.
std::string ReportFailure(ScriptEngine::Impl& impl, const std::string& what,
                          const sol::protected_function_result& result)
{
    if (result.valid())
    {
        return {};
    }

    const sol::error error = result;
    impl.LastError = what + ": " + error.what();
    UHE_CORE_ERROR("[script] {0}: {1}", what, error.what());
    return impl.LastError;
}

/// Disables an instance and records why, both for it and as the engine's last error.
void MarkErrored(ScriptEngine::Impl& impl, ScriptEngine::Impl::Instance& instance, const std::string& message)
{
    instance.Errored = true;
    instance.Error = message;
    impl.LastError = message;
}

/// Erases every instance released while a hook was running.
void FlushReleased(ScriptEngine::Impl& impl)
{
    // Erasing destroys sol references, which never calls back into a script,
    // so the list cannot grow while this runs.
    for (const u32 id : impl.PendingRelease)
    {
        impl.Instances.erase(id);
    }
    impl.PendingRelease.clear();
}

/// Brackets every call into a script. While one is on the stack an instance
/// is never erased, only marked dead: the hook that is running may belong to
/// it (`self:Destroy()` in OnUpdate), and sol still reads the
/// protected_function it was called through after the call returns.
class HookScope
{
public:
    explicit HookScope(ScriptEngine::Impl& impl) : m_Impl(impl)
    {
        if (m_Impl.HookDepth++ == 0)
        {
            m_Impl.InstructionsUsed = 0; // the budget is per outermost call
        }
    }
    ~HookScope()
    {
        if (--m_Impl.HookDepth == 0)
        {
            FlushReleased(m_Impl);
        }
    }
    HookScope(const HookScope&) = delete;
    HookScope& operator=(const HookScope&) = delete;

private:
    ScriptEngine::Impl& m_Impl;
};

void StopInstance(ScriptEngine::Impl& impl, ScriptEngine::Impl::Instance& instance);

/// Stops an instance (OnDestroy) and removes it, now or, when a script call is
/// on the stack, once the outermost one returns. A dead instance is already
/// stopped, so releasing it twice is a no-op.
void ReleaseInstance(ScriptEngine::Impl& impl, u32 id)
{
    const auto it = impl.Instances.find(id);
    if (it == impl.Instances.end() || it->second.Dead)
    {
        return;
    }

    // Held across the whole release, not just OnDestroy: the script may release
    // this same instance again (an OnDestroy that destroys its own entity), and
    // `it` must stay valid until the erase this scope performs on exit.
    HookScope scope(impl);
    StopInstance(impl, it->second);
    if (it->second.Dead)
    {
        return;
    }
    it->second.Dead = true;
    impl.PendingRelease.push_back(id);
}

/// Compiles and runs one instance's source, then collects its hooks.
bool LoadInstance(ScriptEngine::Impl& impl, ScriptEngine::Impl::Instance& instance, std::string& error)
{
    const std::string resolved = ResolveScriptPath(instance.Path);
    std::string source;
    if (!ReadFile(resolved, source))
    {
        error = "cannot read '" + resolved + "'";
        return false;
    }

    // The chunk runs in its own environment, so two scripts' globals cannot
    // collide and only what the script defines is visible to the engine.
    sol::environment env(impl.Lua, sol::create, impl.SharedGlobals);
    env["_G"] = env; // `_G.x = 1` stays inside this script
    // Text only: Lua does not verify precompiled bytecode, and a crafted binary
    // chunk can corrupt the VM, which would make the sandbox meaningless.
    sol::load_result loaded = impl.Lua.load(source, "@" + resolved, sol::load_mode::text);
    if (!loaded.valid())
    {
        const sol::error loadError = loaded;
        error = loadError.what();
        return false;
    }

    sol::protected_function chunk = loaded;
    env.set_on(chunk);
    if (impl.Traceback.valid())
    {
        chunk.set_error_handler(impl.Traceback);
    }

    sol::protected_function_result ran;
    {
        HookScope scope(impl);
        ran = chunk();
    }
    if (!ran.valid())
    {
        const sol::error runError = ran;
        error = runError.what();
        return false;
    }

    instance.Self = MakeEntityTable(impl, *instance.Scene_, instance.Entity);
    instance.OnCreate = HookOf(impl, env, "OnCreate");
    instance.OnUpdate = HookOf(impl, env, "OnUpdate");
    instance.OnDestroy = HookOf(impl, env, "OnDestroy");
    instance.ResolvedPath = resolved;
    instance.SourceTime = ModificationTime(resolved);
    return true;
}

/// \returns the instance for this entity, creating (or restarting) it when the
/// component is new, its path changed, or its instance id no longer exists.
ScriptEngine::Impl::Instance* EnsureInstance(ScriptEngine::Impl& impl, Scene& scene, entt::entity handle,
                                             ScriptComponent& component)
{
    if (component.ScriptPath.empty())
    {
        if (component.RuntimeIndex != kInvalidInstance)
        {
            ReleaseInstance(impl, component.RuntimeIndex);
        }
        component.RuntimeIndex = kInvalidInstance;
        return nullptr;
    }

    const auto existing = impl.Instances.find(component.RuntimeIndex);
    if (existing != impl.Instances.end() && !existing->second.Dead && existing->second.Scene_ == &scene &&
        existing->second.Entity == handle && existing->second.Path == component.ScriptPath)
    {
        return &existing->second;
    }

    // A component whose path changed gets a fresh instance: state belongs to
    // the instance, not to the component. The old one is stopped first, so its
    // OnDestroy runs like any other end of life. An instance that belongs to
    // another entity or scene (the component was copied) is left alone: it is
    // still that entity's script.
    if (existing != impl.Instances.end() && existing->second.Scene_ == &scene && existing->second.Entity == handle)
    {
        ReleaseInstance(impl, existing->first);
    }

    const u32 id = impl.NextInstanceId++;
    ScriptEngine::Impl::Instance& instance = impl.Instances[id];
    instance.Scene_ = &scene;
    instance.Entity = handle;
    instance.Path = component.ScriptPath;
    component.RuntimeIndex = id;

    std::string error;
    if (!LoadInstance(impl, instance, error))
    {
        MarkErrored(impl, instance, DescribeEntity(scene, handle) + " (" + component.ScriptPath + "): " + error);
        UHE_CORE_ERROR("[script] {0}", instance.Error);
    }
    return &instance;
}

/// Calls `OnCreate` once. A failure disables the instance.
void StartInstance(ScriptEngine::Impl& impl, ScriptEngine::Impl::Instance& instance)
{
    instance.Started = true;
    if (instance.Errored || !instance.OnCreate.valid())
    {
        return;
    }

    sol::protected_function_result result;
    {
        HookScope scope(impl);
        result = instance.OnCreate(instance.Self);
    }
    if (!result.valid())
    {
        MarkErrored(impl, instance,
                    ReportFailure(impl, DescribeEntity(*instance.Scene_, instance.Entity) + " OnCreate", result));
    }
}

void StopInstance(ScriptEngine::Impl& impl, ScriptEngine::Impl::Instance& instance)
{
    // Cleared before OnDestroy runs: an OnDestroy that destroys its own entity
    // comes back here, and must not run OnDestroy a second time.
    const bool wasStarted = instance.Started;
    instance.Started = false;
    if (wasStarted && !instance.Errored && instance.OnDestroy.valid())
    {
        sol::protected_function_result result;
        {
            HookScope scope(impl);
            result = instance.OnDestroy(instance.Self);
        }
        if (!result.valid())
        {
            ReportFailure(impl, DescribeEntity(*instance.Scene_, instance.Entity) + " OnDestroy", result);
        }
    }
}

/// Recompiles one instance and runs its lifecycle again, which is the whole of
/// hot reload: state is lost, and that is documented behaviour.
void RestartInstance(ScriptEngine::Impl& impl, u32 id)
{
    const auto it = impl.Instances.find(id);
    if (it == impl.Instances.end() || it->second.Dead)
    {
        return;
    }

    ScriptEngine::Impl::Instance& instance = it->second;
    StopInstance(impl, instance);
    if (instance.Dead)
    {
        return; // its OnDestroy destroyed the entity
    }

    instance.Errored = false;
    instance.Error.clear();
    std::string error;
    if (!LoadInstance(impl, instance, error))
    {
        MarkErrored(impl, instance,
                    DescribeEntity(*instance.Scene_, instance.Entity) + " (" + instance.Path + "): " + error);
        UHE_CORE_ERROR("[script] {0}", instance.Error);
        return;
    }

    ++instance.ReloadCount;
    UHE_CORE_INFO("[script] reloaded {0} for {1}", instance.Path, DescribeEntity(*instance.Scene_, instance.Entity));
    StartInstance(impl, instance);
}

void BindApi(ScriptEngine::Impl& impl)
{
    sol::state& lua = impl.Lua;

    // ── math types ──────────────────────────────────────────────────────────
    lua.new_usertype<Vec3>("Vec3", sol::no_constructor, "x", &Vec3::x, "y", &Vec3::y, "z", &Vec3::z, "Length",
                           &Vec3::Length);

    // ── logging ─────────────────────────────────────────────────────────────
    sol::table log = lua.create_named_table("Log");
    log.set_function("info", [](const std::string& message) { UHE_CORE_INFO("[script] {0}", message); });
    log.set_function("warn", [](const std::string& message) { UHE_CORE_WARN("[script] {0}", message); });
    log.set_function("error", [](const std::string& message) { UHE_CORE_ERROR("[script] {0}", message); });

    // ── input ───────────────────────────────────────────────────────────────
    sol::table input = lua.create_named_table("Input");
    input.set_function("IsKeyPressed", [](int key) { return HasWindow() && Input::IsKeyPressed(key); });
    input.set_function("IsMouseButtonPressed",
                       [](int button) { return HasWindow() && Input::IsMouseButtonPressed(button); });
    input.set_function("GetMousePosition",
                       []
                       {
                           if (!HasWindow())
                           {
                               return Vec3{};
                           }
                           const auto [x, y] = Input::GetMousePosition();
                           return Vec3{x, y, 0.0f};
                       });

    // ── scene ───────────────────────────────────────────────────────────────
    lua.set_function("Spawn",
                     [&impl](const std::string& name) -> sol::object
                     {
                         if (impl.ActiveScene == nullptr)
                         {
                             return sol::nil;
                         }
                         return sol::make_object(
                             impl.Lua.lua_state(),
                             MakeEntityTable(impl, *impl.ActiveScene, impl.ActiveScene->CreateEntity(name)));
                     });

    // ── entity methods, shared by every instance table ──────────────────────
    sol::table methods = lua.create_table();

    methods.set_function("IsValid",
                         [&impl](sol::table self)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             return ResolveEntity(impl, self, scene, handle);
                         });

    methods.set_function("GetName",
                         [&impl](sol::table self) -> std::string
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return {};
                             }
                             return DescribeEntity(*scene, handle);
                         });

    methods.set_function("SetName",
                         [&impl](sol::table self, const std::string& name)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return;
                             }
                             Entity entity{handle, scene};
                             if (entity.HasComponent<TagComponent>())
                             {
                                 entity.GetComponent<TagComponent>().Tag = name;
                             }
                         });

    // A string, not a number: the id is a random 64-bit UUID, and a Lua integer
    // is signed, so half of them cannot be represented. Scripts compare ids, they
    // do not do arithmetic on them.
    methods.set_function("GetID",
                         [&impl](sol::table self) -> std::string
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return {};
                             }
                             Entity entity{handle, scene};
                             if (!entity.HasComponent<IDComponent>())
                             {
                                 return {};
                             }
                             return std::to_string(entity.GetComponent<IDComponent>().ID);
                         });

    methods.set_function("GetPosition",
                         [&impl](sol::table self) -> sol::object
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return sol::nil;
                             }
                             Entity entity{handle, scene};
                             if (!entity.HasComponent<TransformComponent>())
                             {
                                 return sol::nil;
                             }
                             return sol::make_object(self.lua_state(),
                                                     ToVec3(entity.GetComponent<TransformComponent>().Translation));
                         });

    methods.set_function("SetPosition",
                         [&impl](sol::table self, float x, float y, float z)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return;
                             }
                             Entity entity{handle, scene};
                             if (entity.HasComponent<TransformComponent>())
                             {
                                 entity.GetComponent<TransformComponent>().Translation = {x, y, z};
                             }
                         });

    methods.set_function("Translate",
                         [&impl](sol::table self, float dx, float dy, float dz)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return;
                             }
                             Entity entity{handle, scene};
                             if (entity.HasComponent<TransformComponent>())
                             {
                                 entity.GetComponent<TransformComponent>().Translation += glm::vec3(dx, dy, dz);
                             }
                         });

    methods.set_function("GetRotation",
                         [&impl](sol::table self) -> sol::object
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return sol::nil;
                             }
                             Entity entity{handle, scene};
                             if (!entity.HasComponent<TransformComponent>())
                             {
                                 return sol::nil;
                             }
                             return sol::make_object(self.lua_state(),
                                                     ToVec3(entity.GetComponent<TransformComponent>().Rotation));
                         });

    methods.set_function("SetRotation",
                         [&impl](sol::table self, float x, float y, float z)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return;
                             }
                             Entity entity{handle, scene};
                             if (entity.HasComponent<TransformComponent>())
                             {
                                 entity.GetComponent<TransformComponent>().Rotation = {x, y, z};
                             }
                         });

    methods.set_function("GetScale",
                         [&impl](sol::table self) -> sol::object
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return sol::nil;
                             }
                             Entity entity{handle, scene};
                             if (!entity.HasComponent<TransformComponent>())
                             {
                                 return sol::nil;
                             }
                             return sol::make_object(self.lua_state(),
                                                     ToVec3(entity.GetComponent<TransformComponent>().Scale));
                         });

    methods.set_function("SetScale",
                         [&impl](sol::table self, float x, float y, float z)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return;
                             }
                             Entity entity{handle, scene};
                             if (entity.HasComponent<TransformComponent>())
                             {
                                 entity.GetComponent<TransformComponent>().Scale = {x, y, z};
                             }
                         });

    methods.set_function("Destroy",
                         [&impl](sol::table self)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return;
                             }
                             // Immediate: the entity's script stops here, and any
                             // later call on this table answers "invalid".
                             scene->DestroyEntity(Entity{handle, scene});
                         });

    // ── rigid bodies, which is what makes a script a controller ─────────────
    methods.set_function("HasRigidBody",
                         [&impl](sol::table self)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return false;
                             }
                             Entity entity{handle, scene};
                             return entity.HasComponent<RigidBody3DComponent>() &&
                                    entity.GetComponent<RigidBody3DComponent>().RuntimeBodyID != kInvalidBodyId;
                         });

    methods.set_function("GetLinearVelocity",
                         [&impl](sol::table self) -> sol::object
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return sol::nil;
                             }
                             JPH::BodyID bodyId;
                             JPH::BodyInterface* bodies = BodyInterfaceOf(*scene, handle, bodyId);
                             if (bodies == nullptr)
                             {
                                 return sol::nil;
                             }
                             return sol::make_object(self.lua_state(),
                                                     ToVec3(ToGlm(bodies->GetLinearVelocity(bodyId))));
                         });

    methods.set_function("SetLinearVelocity",
                         [&impl](sol::table self, float x, float y, float z)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return;
                             }
                             JPH::BodyID bodyId;
                             JPH::BodyInterface* bodies = BodyInterfaceOf(*scene, handle, bodyId);
                             if (bodies == nullptr)
                             {
                                 return;
                             }
                             // A body that is asleep ignores a velocity change.
                             bodies->ActivateBody(bodyId);
                             bodies->SetLinearVelocity(bodyId, ToJolt(glm::vec3(x, y, z)));
                         });

    methods.set_function("GetAngularVelocity",
                         [&impl](sol::table self) -> sol::object
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return sol::nil;
                             }
                             JPH::BodyID bodyId;
                             JPH::BodyInterface* bodies = BodyInterfaceOf(*scene, handle, bodyId);
                             if (bodies == nullptr)
                             {
                                 return sol::nil;
                             }
                             return sol::make_object(self.lua_state(),
                                                     ToVec3(ToGlm(bodies->GetAngularVelocity(bodyId))));
                         });

    methods.set_function("SetAngularVelocity",
                         [&impl](sol::table self, float x, float y, float z)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return;
                             }
                             JPH::BodyID bodyId;
                             JPH::BodyInterface* bodies = BodyInterfaceOf(*scene, handle, bodyId);
                             if (bodies == nullptr)
                             {
                                 return;
                             }
                             bodies->ActivateBody(bodyId);
                             bodies->SetAngularVelocity(bodyId, ToJolt(glm::vec3(x, y, z)));
                         });

    methods.set_function("ApplyForce",
                         [&impl](sol::table self, float x, float y, float z)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return;
                             }
                             JPH::BodyID bodyId;
                             JPH::BodyInterface* bodies = BodyInterfaceOf(*scene, handle, bodyId);
                             if (bodies == nullptr)
                             {
                                 return;
                             }
                             bodies->ActivateBody(bodyId);
                             bodies->AddForce(bodyId, ToJolt(glm::vec3(x, y, z)));
                         });

    // ── animation graph parameters ─────────────────────────────────────────
    methods.set_function("HasAnimator",
                         [&impl](sol::table self)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                                 return false;
                             Entity entity{handle, scene};
                             return entity.HasComponent<AnimatorComponent>() &&
                                    entity.GetComponent<AnimatorComponent>().Animator != nullptr;
                         });

    methods.set_function("SetAnimFloat",
                         [&impl](sol::table self, const std::string& name, float value)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                                 return;
                             Entity entity{handle, scene};
                             if (!entity.HasComponent<AnimatorComponent>())
                                 return;

                             AnimationGraph& graph = entity.GetComponent<AnimatorComponent>().Graph;
                             if (graph.FindParameter(name) < 0)
                                 graph.Parameters.push_back({name, value});
                             else
                                 graph.SetParameter(name, value);
                         });

    methods.set_function("GetAnimFloat",
                         [&impl](sol::table self, const std::string& name) -> float
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                                 return 0.0f;
                             Entity entity{handle, scene};
                             if (!entity.HasComponent<AnimatorComponent>())
                                 return 0.0f;
                             return entity.GetComponent<AnimatorComponent>().Graph.GetParameter(name);
                         });

    methods.set_function("SetAnimTrigger",
                         [&impl](sol::table self, const std::string& name)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                                 return;
                             Entity entity{handle, scene};
                             if (!entity.HasComponent<AnimatorComponent>())
                                 return;

                             AnimationGraph& graph = entity.GetComponent<AnimatorComponent>().Graph;
                             if (graph.FindParameter(name) < 0)
                                 graph.Parameters.push_back({name, 1.0f});
                             else
                                 graph.SetParameter(name, 1.0f);
                         });

    // Direct state jump: useful for robots whose controller already decided
    // the state, bypassing transition conditions for one frame.
    methods.set_function("SetAnimState",
                         [&impl](sol::table self, const std::string& stateName)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                                 return;
                             Entity entity{handle, scene};
                             if (!entity.HasComponent<AnimatorComponent>())
                                 return;

                             AnimatorComponent& anim = entity.GetComponent<AnimatorComponent>();
                             for (const AnimationGraphState& state : anim.Graph.States)
                             {
                                 if (state.Name == stateName)
                                 {
                                     if (anim.GraphRuntime && anim.Animator)
                                         anim.GraphRuntime->Enter(stateName, *anim.Animator, 0.0f);
                                     if (!state.Clip.empty())
                                         anim.CurrentAnimationName = state.Clip;
                                     return;
                                 }
                             }
                         });

    methods.set_function("PlayAnimation",
                         [&impl](sol::table self, const std::string& clip, float fade)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                                 return;
                             Entity entity{handle, scene};
                             if (!entity.HasComponent<AnimatorComponent>())
                                 return;

                             AnimatorComponent& anim = entity.GetComponent<AnimatorComponent>();
                             if (!anim.Animator)
                                 return;

                             if (anim.Animator->HasAnimation() && fade > 0.0f)
                                 anim.Animator->CrossFade(clip, fade);
                             else
                                 anim.Animator->PlayAnimation(clip);
                             anim.CurrentAnimationName = clip;
                             anim.IsPlaying = true;
                         });

    impl.EntityMethods = methods;

    lua.set_function("Find",
                     [&impl](const std::string& name) -> sol::object
                     {
                         if (impl.ActiveScene == nullptr)
                         {
                             return sol::nil;
                         }

                         Scene* scene = impl.ActiveScene;
                         auto view = scene->GetRegistry().view<TagComponent>();
                         for (const entt::entity handle : view)
                         {
                             if (view.get<TagComponent>(handle).Tag == name)
                             {
                                 return sol::make_object(impl.Lua.lua_state(), MakeEntityTable(impl, *scene, handle));
                             }
                         }
                         return sol::nil;
                     });
}
} // namespace

ScriptEngine::ScriptEngine() : m_Impl(std::make_unique<Impl>()) {}

ScriptEngine::~ScriptEngine() = default;

ScriptEngine& ScriptEngine::Get()
{
    static ScriptEngine engine;
    return engine;
}

void ScriptEngine::Init()
{
    if (m_Impl->Initialized)
    {
        return;
    }

    // Only the libraries a game script needs. `io`, `os`, `package` and `debug`
    // are never opened, so a script cannot read the disk, spawn a process or
    // reach into the VM.
    m_Impl->Lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::utf8,
                               sol::lib::coroutine, sol::lib::debug);

    // `debug.traceback` is the one thing worth keeping from the debug library:
    // an error without a traceback is guesswork. Capture it, then take the
    // library away again.
    const sol::object debug = m_Impl->Lua["debug"];
    if (debug.is<sol::table>())
    {
        const sol::object traceback = debug.as<sol::table>()["traceback"];
        if (traceback.is<sol::function>())
        {
            m_Impl->Traceback = traceback.as<sol::function>();
        }
    }
    m_Impl->Lua["debug"] = sol::nil;

    // Code loading is not part of the API: scripts are files the engine loads.
    m_Impl->Lua["load"] = sol::nil;
    m_Impl->Lua["loadfile"] = sol::nil;
    m_Impl->Lua["dofile"] = sol::nil;
    m_Impl->Lua["require"] = sol::nil;
    m_Impl->Lua["collectgarbage"] = sol::nil;
    m_Impl->Lua["print"] = sol::nil; // Log.info is the way out

    // An unrecoverable VM error means the host used the state outside a protected
    // call, which is a bug here rather than in a script: every script call is
    // protected. Lua unwinds by longjmp, so returning from this handler is not an
    // option -- it would unwind through C++ frames. Report and stop.
    m_Impl->Lua.set_panic(
        [](lua_State* state) -> int
        {
            const char* message = lua_tostring(state, -1);
            UHE_CORE_CRITICAL("[script] unrecoverable Lua error: {0}", message != nullptr ? message : "(no message)");
            std::abort();
        });

    BindApi(*m_Impl);

    // Seal the shared globals last, so the engine API is sealed with them.
    {
        sol::protected_function_result sealed =
            m_Impl->Lua.safe_script(kSealGlobals, sol::script_pass_on_error, "=seal", sol::load_mode::text);
        if (sealed.valid() && sealed.get_type() == sol::type::table)
        {
            m_Impl->SharedGlobals = sealed;
        }
        else
        {
            UHE_CORE_ERROR("[script] could not seal the globals; scripts share a writable _G");
            m_Impl->SharedGlobals = m_Impl->Lua.globals();
        }
    }

    // Every call gets an instruction budget; the hook is per state and coroutines inherit it.
    OwnerOf(m_Impl->Lua.lua_state()) = m_Impl.get();
    lua_sethook(m_Impl->Lua.lua_state(), &BudgetHook, LUA_MASKCOUNT, kBudgetCheckInterval);

    m_Impl->Initialized = true;
    UHE_CORE_INFO("Scripting initialized (Lua {0})", LUA_VERSION);
}

void ScriptEngine::Shutdown()
{
    if (!m_Impl->Initialized)
    {
        return;
    }

    std::vector<u32> ids;
    ids.reserve(m_Impl->Instances.size());
    for (const auto& [id, instance] : m_Impl->Instances)
    {
        (void)instance;
        ids.push_back(id);
    }
    for (const u32 id : ids)
    {
        ReleaseInstance(*m_Impl, id);
    }
    m_Impl->Instances.clear();
    m_Impl->PendingRelease.clear();
    m_Impl->EntityMethods = sol::table();
    m_Impl->Traceback = sol::function();
    m_Impl->SharedGlobals = sol::table();
    m_Impl->ActiveScene = nullptr;

    // Close the VM here, not when the static ScriptEngine dies at exit: by then
    // sol's own statics (the usertype names its finalizers read) are already
    // destroyed, and closing the state reads freed memory. Every reference into
    // the old state is released above, so replacing it is safe; Init() opens
    // the libraries again on the fresh one.
    m_Impl->Lua = sol::state();
    m_Impl->Initialized = false;
}

bool ScriptEngine::IsInitialized() const
{
    return m_Impl->Initialized;
}

lua_State* ScriptEngine::State() const
{
    return m_Impl->Initialized ? m_Impl->Lua.lua_state() : nullptr;
}

const std::string& ScriptEngine::LastError() const
{
    return m_Impl->LastError;
}

void ScriptEngine::SetInstructionBudget(u64 instructions)
{
    m_Impl->InstructionBudget = instructions;
}

u64 ScriptEngine::GetInstructionBudget() const
{
    return m_Impl->InstructionBudget;
}

u32 ScriptEngine::InstanceCount() const
{
    u32 count = 0;
    for (const auto& [id, instance] : m_Impl->Instances)
    {
        (void)id;
        count += instance.Dead ? 0u : 1u;
    }
    return count;
}

u32 ScriptEngine::ErroredInstanceCount() const
{
    u32 count = 0;
    for (const auto& [id, instance] : m_Impl->Instances)
    {
        (void)id;
        count += (instance.Errored && !instance.Dead) ? 1u : 0u;
    }
    return count;
}

bool ScriptEngine::Run(const std::string& source, const std::string& chunkName)
{
    if (!m_Impl->Initialized)
    {
        return false;
    }

    sol::protected_function_result result;
    {
        HookScope scope(*m_Impl);
        result = m_Impl->Lua.safe_script(source, sol::script_pass_on_error, chunkName, sol::load_mode::text);
    }
    if (!result.valid())
    {
        ReportFailure(*m_Impl, chunkName, result);
        return false;
    }
    return true;
}

bool ScriptEngine::RunFile(const std::string& path)
{
    const std::string resolved = ResolveScriptPath(path);
    std::string source;
    if (!ReadFile(resolved, source))
    {
        m_Impl->LastError = "cannot read '" + resolved + "'";
        UHE_CORE_ERROR("[script] {0}", m_Impl->LastError);
        return false;
    }
    return Run(source, resolved);
}

void ScriptEngine::OnSceneStart(Scene& scene)
{
    if (!m_Impl->Initialized)
    {
        return;
    }

    m_Impl->ActiveScene = &scene;
    auto view = scene.GetRegistry().view<ScriptComponent>();
    for (const entt::entity handle : view)
    {
        ScriptComponent& component = view.get<ScriptComponent>(handle);
        Impl::Instance* instance = EnsureInstance(*m_Impl, scene, handle, component);
        if (instance != nullptr && !instance->Started && !instance->Errored)
        {
            StartInstance(*m_Impl, *instance);
        }
    }
}

void ScriptEngine::Update(Scene& scene, Timestep ts)
{
    if (!m_Impl->Initialized)
    {
        return;
    }

    m_Impl->ActiveScene = &scene;

    // A snapshot: a script may spawn or destroy entities while this runs, and an
    // entt view must not be iterated across either.
    std::vector<entt::entity> entities;
    auto view = scene.GetRegistry().view<ScriptComponent>();
    entities.reserve(view.size());
    for (const entt::entity handle : view)
    {
        entities.push_back(handle);
    }

    for (const entt::entity handle : entities)
    {
        if (!scene.GetRegistry().valid(handle))
        {
            continue;
        }

        ScriptComponent& component = scene.GetRegistry().get<ScriptComponent>(handle);
        Impl::Instance* instance = EnsureInstance(*m_Impl, scene, handle, component);
        if (instance == nullptr || instance->Dead || instance->Errored || !component.Enabled)
        {
            continue;
        }

        if (!instance->Started)
        {
            StartInstance(*m_Impl, *instance);
            continue; // OnCreate and OnUpdate are not the same frame
        }

        if (!instance->OnUpdate.valid())
        {
            continue;
        }

        sol::protected_function_result result;
        {
            // The script may destroy its own entity here. The instance then
            // stays allocated (dead) until this scope closes, so `instance`
            // is still valid below.
            HookScope scope(*m_Impl);
            result = instance->OnUpdate(instance->Self, ts.GetSeconds());
            if (!result.valid())
            {
                MarkErrored(*m_Impl, *instance,
                            ReportFailure(*m_Impl, DescribeEntity(scene, handle) + " OnUpdate", result));
            }
        }
    }
}

void ScriptEngine::OnSceneStop(Scene& scene)
{
    if (!m_Impl->Initialized)
    {
        return;
    }

    std::vector<u32> ids;
    for (const auto& [id, instance] : m_Impl->Instances)
    {
        if (instance.Scene_ == &scene)
        {
            ids.push_back(id);
        }
    }
    for (const u32 id : ids)
    {
        ReleaseInstance(*m_Impl, id);
    }

    if (m_Impl->ActiveScene == &scene)
    {
        m_Impl->ActiveScene = nullptr;
    }
}

void ScriptEngine::OnEntityDestroyed(Scene& scene, u32 entityId)
{
    if (!m_Impl->Initialized)
    {
        return;
    }

    const entt::entity handle = static_cast<entt::entity>(entityId);
    for (const auto& [id, instance] : m_Impl->Instances)
    {
        if (!instance.Dead && instance.Scene_ == &scene && instance.Entity == handle)
        {
            // Copy the id: releasing may erase the element this loop reads.
            const u32 found = id;
            ReleaseInstance(*m_Impl, found);
            return;
        }
    }
}

void ScriptEngine::SetHotReloadEnabled(bool enabled)
{
    m_Impl->HotReload = enabled;
    m_Impl->LastPollSeconds = -1.0;
    UHE_CORE_INFO("Script hot reload {0}", enabled ? "enabled" : "disabled");
}

bool ScriptEngine::IsHotReloadEnabled() const
{
    return m_Impl->HotReload;
}

void ScriptEngine::Tick(Scene& scene)
{
    if (!m_Impl->Initialized || !m_Impl->HotReload || m_Impl->Instances.empty())
    {
        return;
    }

    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m_Impl->LastPollSeconds >= 0.0 && now - m_Impl->LastPollSeconds < kHotReloadPollSeconds)
    {
        return;
    }
    m_Impl->LastPollSeconds = now;

    std::vector<u32> changed;
    for (const auto& [id, instance] : m_Impl->Instances)
    {
        if (instance.Dead || instance.Scene_ != &scene || instance.ResolvedPath.empty())
        {
            continue;
        }
        if (ModificationTime(instance.ResolvedPath) > instance.SourceTime)
        {
            changed.push_back(id);
        }
    }

    for (const u32 id : changed)
    {
        RestartInstance(*m_Impl, id);
    }
}

bool ScriptEngine::ReloadEntity(Scene& scene, u32 entityId)
{
    if (!m_Impl->Initialized)
    {
        return false;
    }

    const entt::entity handle = static_cast<entt::entity>(entityId);
    if (!scene.GetRegistry().valid(handle) || !scene.GetRegistry().all_of<ScriptComponent>(handle))
    {
        return false;
    }

    ScriptComponent& component = scene.GetRegistry().get<ScriptComponent>(handle);
    Impl::Instance* instance = EnsureInstance(*m_Impl, scene, handle, component);
    if (instance == nullptr)
    {
        return false;
    }

    // A failed load left the instance errored and never started; restarting it
    // has to clear that, or the editor's Reload button would do nothing.
    RestartInstance(*m_Impl, component.RuntimeIndex);
    return true;
}

std::vector<ScriptEngine::InstanceInfo> ScriptEngine::Instances(Scene& scene) const
{
    std::vector<InstanceInfo> infos;
    if (!m_Impl->Initialized)
    {
        return infos;
    }

    // Walk the scene rather than the instance map so the editor sees the order
    // it draws entities in, and so a component with no instance yet still shows.
    auto view = scene.GetRegistry().view<ScriptComponent>();
    for (const entt::entity handle : view)
    {
        const ScriptComponent& component = view.get<ScriptComponent>(handle);

        InstanceInfo info;
        info.EntityId = static_cast<u32>(handle);
        info.Path = component.ScriptPath;
        {
            Entity entity{handle, &scene};
            info.EntityName =
                entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag : std::string("<entity>");
        }

        const auto it = m_Impl->Instances.find(component.RuntimeIndex);
        if (it != m_Impl->Instances.end() && !it->second.Dead && it->second.Scene_ == &scene &&
            it->second.Entity == handle)
        {
            info.Started = it->second.Started;
            info.Errored = it->second.Errored;
            info.ReloadCount = it->second.ReloadCount;
            if (it->second.Errored)
            {
                info.Error = it->second.Error;
            }
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

void ScriptEngine::ReloadAll(Scene& scene)
{
    if (!m_Impl->Initialized)
    {
        return;
    }

    std::vector<u32> ids;
    for (const auto& [id, instance] : m_Impl->Instances)
    {
        if (!instance.Dead && instance.Scene_ == &scene)
        {
            ids.push_back(id);
        }
    }

    for (const u32 id : ids)
    {
        RestartInstance(*m_Impl, id);
    }
}
} // namespace UHE
