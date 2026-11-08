#include "uhepch.h"
#include "ScriptEngine.h"

#include "UHE/AssestsManager/VfsSystem.h"
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

Vec3 ToVec3(const glm::vec3& value) { return {value.x, value.y, value.z}; }
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
        u32 ReloadCount = 0;
    };

    sol::state Lua;
    bool Initialized = false;
    bool HotReload = false;
    double LastPollSeconds = -1.0;
    std::string LastError;
    u32 NextInstanceId = 1;
    std::unordered_map<u32, Instance> Instances;
    sol::table EntityMethods;
    sol::function Traceback;
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
    return scene != nullptr && scene == impl.ActiveScene && handle != entt::null &&
           scene->GetRegistry().valid(handle);
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
void ReportFailure(ScriptEngine::Impl& impl, const std::string& what, const sol::protected_function_result& result)
{
    std::string message = "script error";
    if (result.valid())
    {
        return;
    }

    const sol::error error = result;
    message = error.what();

    impl.LastError = what + ": " + message;
    UHE_CORE_ERROR("[script] {0}: {1}", what, message);
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
    sol::environment env(impl.Lua, sol::create, impl.Lua.globals());
    sol::load_result loaded = impl.Lua.load(source, "@" + resolved);
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

    const sol::protected_function_result ran = chunk();
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
        component.RuntimeIndex = kInvalidInstance;
        return nullptr;
    }

    const auto existing = impl.Instances.find(component.RuntimeIndex);
    if (existing != impl.Instances.end() && existing->second.Scene_ == &scene &&
        existing->second.Entity == handle && existing->second.Path == component.ScriptPath)
    {
        return &existing->second;
    }

    // A component that was copied, retargeted or whose instance was torn down
    // gets a fresh instance: state belongs to the instance, not to the component.
    if (existing != impl.Instances.end())
    {
        impl.Instances.erase(existing);
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
        instance.Errored = true;
        impl.LastError = DescribeEntity(scene, handle) + " (" + component.ScriptPath + "): " + error;
        UHE_CORE_ERROR("[script] {0} ({1}): {2}", DescribeEntity(scene, handle), component.ScriptPath, error);
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

    const sol::protected_function_result result = instance.OnCreate(instance.Self);
    if (!result.valid())
    {
        ReportFailure(impl, DescribeEntity(*instance.Scene_, instance.Entity) + " OnCreate", result);
        instance.Errored = true;
    }
}

void StopInstance(ScriptEngine::Impl& impl, ScriptEngine::Impl::Instance& instance)
{
    if (instance.Started && !instance.Errored && instance.OnDestroy.valid())
    {
        const sol::protected_function_result result = instance.OnDestroy(instance.Self);
        if (!result.valid())
        {
            ReportFailure(impl, DescribeEntity(*instance.Scene_, instance.Entity) + " OnDestroy", result);
        }
    }
    instance.Started = false;
}

/// Recompiles one instance and runs its lifecycle again, which is the whole of
/// hot reload: state is lost, and that is documented behaviour.
void RestartInstance(ScriptEngine::Impl& impl, u32 id)
{
    const auto it = impl.Instances.find(id);
    if (it == impl.Instances.end())
    {
        return;
    }

    ScriptEngine::Impl::Instance& instance = it->second;
    StopInstance(impl, instance);

    instance.Errored = false;
    std::string error;
    if (!LoadInstance(impl, instance, error))
    {
        instance.Errored = true;
        impl.LastError = DescribeEntity(*instance.Scene_, instance.Entity) + " (" + instance.Path + "): " + error;
        UHE_CORE_ERROR("[script] {0} ({1}): {2}", DescribeEntity(*instance.Scene_, instance.Entity), instance.Path,
                       error);
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
    lua.set_function("Spawn", [&impl](const std::string& name) -> sol::object
                     {
                         if (impl.ActiveScene == nullptr)
                         {
                             return sol::nil;
                         }
                         return sol::make_object(impl.Lua.lua_state(),
                                                 MakeEntityTable(impl, *impl.ActiveScene,
                                                                 impl.ActiveScene->CreateEntity(name)));
                     });

    // ── entity methods, shared by every instance table ──────────────────────
    sol::table methods = lua.create_table();

    methods.set_function("IsValid", [&impl](sol::table self)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             return ResolveEntity(impl, self, scene, handle);
                         });

    methods.set_function("GetName", [&impl](sol::table self) -> std::string
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                             {
                                 return {};
                             }
                             return DescribeEntity(*scene, handle);
                         });

    methods.set_function("SetName", [&impl](sol::table self, const std::string& name)
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
    methods.set_function("GetID", [&impl](sol::table self) -> std::string
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

    methods.set_function("GetPosition", [&impl](sol::table self) -> sol::object
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

    methods.set_function("SetPosition", [&impl](sol::table self, float x, float y, float z)
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

    methods.set_function("Translate", [&impl](sol::table self, float dx, float dy, float dz)
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

    methods.set_function("GetRotation", [&impl](sol::table self) -> sol::object
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

    methods.set_function("SetRotation", [&impl](sol::table self, float x, float y, float z)
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

    methods.set_function("GetScale", [&impl](sol::table self) -> sol::object
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

    methods.set_function("SetScale", [&impl](sol::table self, float x, float y, float z)
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

    methods.set_function("Destroy", [&impl](sol::table self)
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
    methods.set_function("HasRigidBody", [&impl](sol::table self)
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

    methods.set_function("GetLinearVelocity", [&impl](sol::table self) -> sol::object
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
                             return sol::make_object(self.lua_state(), ToVec3(ToGlm(bodies->GetLinearVelocity(bodyId))));
                         });

    methods.set_function("SetLinearVelocity", [&impl](sol::table self, float x, float y, float z)
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

    methods.set_function("GetAngularVelocity", [&impl](sol::table self) -> sol::object
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
                             return sol::make_object(self.lua_state(), ToVec3(ToGlm(bodies->GetAngularVelocity(bodyId))));
                         });

    methods.set_function("SetAngularVelocity", [&impl](sol::table self, float x, float y, float z)
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

    methods.set_function("ApplyForce", [&impl](sol::table self, float x, float y, float z)
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
    methods.set_function("HasAnimator", [&impl](sol::table self)
                         {
                             Scene* scene = nullptr;
                             entt::entity handle = entt::null;
                             if (!ResolveEntity(impl, self, scene, handle))
                                 return false;
                             Entity entity{handle, scene};
                             return entity.HasComponent<AnimatorComponent>() &&
                                    entity.GetComponent<AnimatorComponent>().Animator != nullptr;
                         });

    methods.set_function("SetAnimFloat", [&impl](sol::table self, const std::string& name, float value)
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

    methods.set_function("GetAnimFloat", [&impl](sol::table self, const std::string& name) -> float
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

    methods.set_function("SetAnimTrigger", [&impl](sol::table self, const std::string& name)
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
    methods.set_function("SetAnimState", [&impl](sol::table self, const std::string& stateName)
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

    methods.set_function("PlayAnimation", [&impl](sol::table self, const std::string& clip, float fade)
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

    lua.set_function("Find", [&impl](const std::string& name) -> sol::object
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
                                 return sol::make_object(impl.Lua.lua_state(),
                                                         MakeEntityTable(impl, *scene, handle));
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

    m_Impl->Initialized = true;
    UHE_CORE_INFO("Scripting initialized (Lua {0})", LUA_VERSION);
}

void ScriptEngine::Shutdown()
{
    if (!m_Impl->Initialized)
    {
        return;
    }

    for (auto& [id, instance] : m_Impl->Instances)
    {
        (void)id;
        StopInstance(*m_Impl, instance);
    }
    m_Impl->Instances.clear();
    m_Impl->EntityMethods = sol::table();
    m_Impl->Traceback = sol::function();
    m_Impl->Initialized = false;
}

bool ScriptEngine::IsInitialized() const { return m_Impl->Initialized; }

lua_State* ScriptEngine::State() const { return m_Impl->Initialized ? m_Impl->Lua.lua_state() : nullptr; }

const std::string& ScriptEngine::LastError() const { return m_Impl->LastError; }

u32 ScriptEngine::InstanceCount() const { return static_cast<u32>(m_Impl->Instances.size()); }

u32 ScriptEngine::ErroredInstanceCount() const
{
    u32 count = 0;
    for (const auto& [id, instance] : m_Impl->Instances)
    {
        (void)id;
        count += instance.Errored ? 1u : 0u;
    }
    return count;
}

bool ScriptEngine::Run(const std::string& source, const std::string& chunkName)
{
    if (!m_Impl->Initialized)
    {
        return false;
    }

    sol::protected_function_result result = m_Impl->Lua.safe_script(source, sol::script_pass_on_error);
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
        if (instance == nullptr || instance->Errored || !component.Enabled)
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

        const sol::protected_function_result result = instance->OnUpdate(instance->Self, ts.GetSeconds());
        if (!result.valid())
        {
            ReportFailure(*m_Impl, DescribeEntity(scene, handle) + " OnUpdate", result);
            instance->Errored = true;
        }
    }
}

void ScriptEngine::OnSceneStop(Scene& scene)
{
    if (!m_Impl->Initialized)
    {
        return;
    }

    for (auto it = m_Impl->Instances.begin(); it != m_Impl->Instances.end();)
    {
        if (it->second.Scene_ != &scene)
        {
            ++it;
            continue;
        }
        StopInstance(*m_Impl, it->second);
        it = m_Impl->Instances.erase(it);
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
    for (auto it = m_Impl->Instances.begin(); it != m_Impl->Instances.end(); ++it)
    {
        if (it->second.Scene_ == &scene && it->second.Entity == handle)
        {
            StopInstance(*m_Impl, it->second);
            m_Impl->Instances.erase(it);
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

bool ScriptEngine::IsHotReloadEnabled() const { return m_Impl->HotReload; }

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
        if (instance.Scene_ != &scene || instance.ResolvedPath.empty())
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
            info.EntityName = entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag
                                                                  : std::string("<entity>");
        }

        const auto it = m_Impl->Instances.find(component.RuntimeIndex);
        if (it != m_Impl->Instances.end() && it->second.Scene_ == &scene && it->second.Entity == handle)
        {
            info.Started = it->second.Started;
            info.Errored = it->second.Errored;
            info.ReloadCount = it->second.ReloadCount;
            if (it->second.Errored)
            {
                info.Error = m_Impl->LastError;
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
        if (instance.Scene_ == &scene)
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
