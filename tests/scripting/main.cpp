// Scripting harness (docs/architecture/scripting.md).
//
// Runs the real ScriptEngine against a real Scene with no window, no device and
// no ImGui -- the property the robotics path depends on, and the only way the
// lifecycle, the error handling and hot reload are testable at all.
//
// What it checks: the sandbox (no io/os/package/debug, no load/dofile), the
// lifecycle hooks and the order they run in, `self` keeping its fields across
// frames, the entity API a script drives (transform, name, find, spawn,
// destroy), a controller loop that converges on a target, an error being logged
// and disabling one instance without touching the frame, a missing file, hot
// reload restarting an instance, `Enabled = false` pausing one, and the script
// surviving a scene save/load round trip.

#include "UHE/Scene/Scene.h"
#include "UHE/Scene/Entity.h"
#include "UHE/Scene/Components.h"
#include "UHE/Scene/SceneSerializer.h"
#include "UHE/Physics/PhysicsSystem3D.h"
#include "UHE/Scripting/ScriptEngine.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace UHE;

namespace
{
int g_failures = 0;

void check(bool condition, const std::string& what)
{
    if (condition)
    {
        std::cout << "  ok   - " << what << '\n';
    }
    else
    {
        std::cout << "  FAIL - " << what << '\n';
        ++g_failures;
    }
}

void section(const std::string& name)
{
    std::cout << "\n[ " << name << " ]\n";
}

std::filesystem::path g_ScriptDir;

/// Writes a script and returns its path. The harness owns its own directory: a
/// test must never edit the project's scripts.
std::string WriteScript(const std::string& name, const std::string& source)
{
    const std::filesystem::path path = g_ScriptDir / name;
    std::ofstream file(path, std::ios::trunc);
    file << source;
    file.close();
    return path.string();
}

glm::vec3 PositionOf(Scene& scene, const std::string& name)
{
    auto view = scene.GetRegistry().view<TagComponent>();
    for (const entt::entity handle : view)
    {
        if (view.get<TagComponent>(handle).Tag == name)
        {
            Entity entity{handle, &scene};
            if (entity.HasComponent<TransformComponent>())
            {
                return entity.GetComponent<TransformComponent>().Translation;
            }
        }
    }
    return {NAN, NAN, NAN};
}

bool HasEntity(Scene& scene, const std::string& name)
{
    auto view = scene.GetRegistry().view<TagComponent>();
    for (const entt::entity handle : view)
    {
        if (view.get<TagComponent>(handle).Tag == name)
        {
            return true;
        }
    }
    return false;
}

/// Starts a scene the way play mode does, then steps it like a runtime frame.
void Start(Scene& scene)
{
    scene.OnRuntimeStart();
    ScriptEngine::Get().OnSceneStart(scene);
}

/// Runs `times` frames of `dt`, each exactly one simulation tick of `dt`: the
/// harness is a headless driver, so it sets the tick to the step it wants
/// instead of feeding frames to the default 120 Hz clock.
void Step(Scene& scene, float dt, int times = 1)
{
    scene.GetSimulationClock().SetFixedStep(dt);
    for (int i = 0; i < times; ++i)
    {
        scene.OnUpdateRuntimeSimulation(Timestep(dt));
    }
}

Entity SpawnScripted(Scene& scene, const std::string& name, const std::string& scriptPath)
{
    Entity entity = scene.CreateEntity(name);
    auto& script = entity.AddComponent<ScriptComponent>();
    script.ScriptPath = scriptPath;
    return entity;
}
} // namespace

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    g_ScriptDir = std::filesystem::temp_directory_path() / "uhe-scripting-test";
    std::error_code ec;
    std::filesystem::remove_all(g_ScriptDir, ec);
    std::filesystem::create_directories(g_ScriptDir, ec);

    // Jolt's global registration, without which creating a scene's physics world
    // walks into an unregistered allocator. Same call the scene harness makes.
    Physics::PhysicsSystem3D::Init();

    ScriptEngine& engine = ScriptEngine::Get();
    engine.Init();

    section("sandbox");
    {
        check(engine.IsInitialized(), "the engine initializes");
        check(engine.State() != nullptr, "and exposes its VM");

        // The libraries a script must not have.
        check(engine.Run("assert(io == nil)", "sandbox"), "io is not reachable");
        check(engine.Run("assert(os == nil)", "sandbox"), "os is not reachable");
        check(engine.Run("assert(package == nil)", "sandbox"), "package is not reachable");
        check(engine.Run("assert(debug == nil)", "sandbox"), "debug is not reachable");
        check(engine.Run("assert(load == nil and loadfile == nil and dofile == nil and require == nil)",
                         "sandbox"),
              "code loading is not reachable");

        // The libraries it does have.
        check(engine.Run("assert(math.floor(2.7) == 2)", "sandbox"), "math works");
        check(engine.Run("assert(string.upper('a') == 'A')", "sandbox"), "string works");
        check(engine.Run("assert(#table.concat({'a','b'}) == 2)", "sandbox"), "table works");
        check(engine.Run("assert(Log ~= nil and Input ~= nil)", "sandbox"), "the engine API is bound");
    }

    section("chunks and errors");
    {
        check(engine.Run("local x = 1 + 1", "good_chunk"), "a good chunk runs");
        check(!engine.Run("error('boom')", "bad_chunk"), "a failing chunk reports failure");
        check(engine.LastError().find("bad_chunk") != std::string::npos,
              "the error names the chunk it came from");
        check(engine.LastError().find("boom") != std::string::npos, "and carries the message");
        check(!engine.RunFile(g_ScriptDir.string() + "/does-not-exist.lua"), "a missing file reports failure");
        check(!engine.LastError().empty(), "and says why");
    }

    section("lifecycle");
    {
        const std::string script = WriteScript("lifecycle.lua", R"(
-- OnCreate runs once; the reset to the origin is what proves it did not run again.
function OnCreate(self)
    self.speed = 2.0
    self:SetPosition(0, 0, 0)
end

function OnUpdate(self, dt)
    self.steps = (self.steps or 0) + 1
    self:Translate(self.speed * dt, 0, 0)
end

function OnDestroy(self)
    local marker = Find("Marker")
    if marker then marker:Translate(0, 1, 0) end
end
)");

        Scene scene;
        SpawnScripted(scene, "Mover", script);
        SpawnScripted(scene, "Marker", WriteScript("marker.lua", R"(
function OnUpdate(self, dt) end
)"));

        Start(scene);
        check(engine.InstanceCount() == 2, "both entities have an instance");

        Step(scene, 0.5f, 2);
        const glm::vec3 afterTwo = PositionOf(scene, "Mover");
        check(std::abs(afterTwo.x - 2.0f) < 1e-4f,
              "OnUpdate ran twice with dt: 2.0 speed * 0.5s * 2 steps");
        check(std::abs(afterTwo.y) < 1e-4f && std::abs(afterTwo.z) < 1e-4f, "and only moved on x");

        // `self` is the instance's own table, so a field written in OnCreate and
        // bumped in OnUpdate survives across frames.
        check(engine.ErroredInstanceCount() == 0, "nothing errored");

        const glm::vec3 markerBefore = PositionOf(scene, "Marker");
        Entity mover = scene.CreateEntity("Lookup");
        (void)mover;
        scene.DestroyEntity([&]
                            {
                                auto view = scene.GetRegistry().view<TagComponent>();
                                for (const entt::entity handle : view)
                                {
                                    if (view.get<TagComponent>(handle).Tag == "Mover")
                                    {
                                        return Entity{handle, &scene};
                                    }
                                }
                                return Entity{};
                            }());

        const glm::vec3 markerAfter = PositionOf(scene, "Marker");
        check(std::abs(markerAfter.y - (markerBefore.y + 1.0f)) < 1e-4f,
              "OnDestroy ran when the entity was destroyed");
        check(engine.InstanceCount() == 1, "the destroyed entity's instance was released");
        check(HasEntity(scene, "Lookup"), "the scene still has its other entities");
    }

    section("entity API");
    {
        const std::string script = WriteScript("api.lua", R"(
function OnCreate(self)
    self:SetName("Renamed")
    self:SetScale(2, 2, 2)
    self:SetRotation(0, 1.5707963, 0)
    local child = Spawn("Child")
    if child then child:SetPosition(5, 0, 0) end
    self.id = self:GetID()
    self.valid = self:IsValid()
end

function OnUpdate(self, dt)
    local found = Find("Child")
    if found then
        self.childX = found:GetPosition().x
    end
    self.name = self:GetName()
    self.scale = self:GetScale().x
    self.mass = self:HasRigidBody()
end
)");

        Scene scene;
        Entity entity = SpawnScripted(scene, "Api", script);
        Start(scene);
        Step(scene, 0.1f, 2);

        check(entity.GetComponent<TagComponent>().Tag == "Renamed", "SetName reached the tag");
        check(std::abs(entity.GetComponent<TransformComponent>().Scale.x - 2.0f) < 1e-4f, "SetScale reached the transform");
        check(HasEntity(scene, "Child"), "Spawn created an entity");
        check(std::abs(PositionOf(scene, "Child").x - 5.0f) < 1e-4f, "and the script positioned it");
        check(entity.GetComponent<IDComponent>().ID != 0 && engine.ErroredInstanceCount() == 0,
              "GetID answered with a value a Lua integer can hold");
        check(engine.ErroredInstanceCount() == 0, "nothing errored");
        check(engine.InstanceCount() == 1, "the spawned entity has no script of its own");
    }

    section("controller loop");
    {
        // A stand-in for the robotics case: a script that steers an entity
        // toward a target, one step at a time, with no renderer involved.
        const std::string script = WriteScript("controller.lua", R"(
local targetX = 10.0
local gain = 0.5

function OnCreate(self)
    self.error = 0.0
end

function OnUpdate(self, dt)
    local p = self:GetPosition()
    local dx = targetX - p.x
    self.error = math.abs(dx)
    local step = dx * gain * dt
    self:Translate(step, 0, 0)
end
)");

        Scene scene;
        SpawnScripted(scene, "Drone", script);
        Start(scene);

        Step(scene, 0.1f, 200);
        const float x = PositionOf(scene, "Drone").x;
        check(x > 9.0f && x <= 10.0f, "the controller converged on its target: x = " + std::to_string(x));
        check(engine.ErroredInstanceCount() == 0, "and ran without error");
    }

    section("errors are contained");
    {
        const std::string broken = WriteScript("broken.lua", R"(
function OnCreate(self)
    self.ok = true
end

function OnUpdate(self, dt)
    error("this script is broken")
end
)");

        Scene scene;
        SpawnScripted(scene, "Broken", broken);
        SpawnScripted(scene, "Healthy", WriteScript("healthy.lua", R"(
function OnUpdate(self, dt)
    self:Translate(1 * dt, 0, 0)
end
)"));

        Start(scene);
        Step(scene, 1.0f, 3);

        check(engine.ErroredInstanceCount() == 1, "the broken instance is disabled");
        check(engine.LastError().find("this script is broken") != std::string::npos, "the error is reported");
        check(engine.LastError().find("broken.lua") != std::string::npos, "with the file it came from");
        check(std::abs(PositionOf(scene, "Healthy").x - 3.0f) < 1e-4f,
              "the healthy instance kept running, on the same frames");
        check(std::abs(PositionOf(scene, "Broken").x) < 1e-4f, "the broken one stopped");

        // A script that fails to load is disabled too, and says why.
        Scene other;
        SpawnScripted(other, "Missing", g_ScriptDir.string() + "/not-here.lua");
        Start(other);
        Step(other, 0.1f);
        check(engine.LastError().find("cannot read") != std::string::npos, "a missing script reports why");
        engine.OnSceneStop(other);
    }

    section("disabled components and hot reload");
    {
        const std::string path = WriteScript("reload.lua", R"(
function OnCreate(self)
    self:SetPosition(0, 0, 0)
end

function OnUpdate(self, dt)
    self:Translate(1 * dt, 0, 0)
end
)");

        Scene scene;
        Entity entity = SpawnScripted(scene, "Reloaded", path);
        Start(scene);
        Step(scene, 1.0f, 2);
        check(std::abs(PositionOf(scene, "Reloaded").x - 2.0f) < 1e-4f, "the script ran");

        // Disabling pauses it without dropping its state.
        entity.GetComponent<ScriptComponent>().Enabled = false;
        Step(scene, 1.0f, 2);
        check(std::abs(PositionOf(scene, "Reloaded").x - 2.0f) < 1e-4f, "a disabled component does not update");
        entity.GetComponent<ScriptComponent>().Enabled = true;
        Step(scene, 1.0f);
        check(std::abs(PositionOf(scene, "Reloaded").x - 3.0f) < 1e-4f, "and resumes where it left off");

        // An edit is what hot reload is for: OnCreate runs again, which resets
        // the position to the origin.
        engine.SetHotReloadEnabled(true);
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5),
                                         ec);
        engine.Tick(scene);
        check(std::abs(PositionOf(scene, "Reloaded").x) < 1e-4f, "a changed script restarts from OnCreate");
        check(engine.ErroredInstanceCount() == 0, "and stays healthy");

        // ReloadAll does the same for an unchanged file.
        Step(scene, 1.0f);
        engine.ReloadAll(scene);
        check(std::abs(PositionOf(scene, "Reloaded").x) < 1e-4f, "an explicit reload restarts it too");
        engine.SetHotReloadEnabled(false);
    }

    section("scene round trip");
    {
        const std::string path = WriteScript("saved.lua", "function OnUpdate(self, dt) end\n");

        Scene scene;
        Entity entity = SpawnScripted(scene, "Saved", path);
        entity.GetComponent<ScriptComponent>().Enabled = false;

        const std::string scenePath = (g_ScriptDir / "round-trip.unhuman").string();
        SceneSerializer serializer(Ref<Scene>(&scene, [](Scene*) {}));
        serializer.Serialize(scenePath);

        Ref<Scene> loaded = CreateRef<Scene>();
        SceneSerializer loader(loaded);
        check(loader.Deserialize(scenePath), "the scene deserializes");

        auto view = loaded->GetRegistry().view<ScriptComponent>();
        check(view.size() == 1, "the script component survived the round trip");
        if (view.size() == 1)
        {
            const auto& component = view.get<ScriptComponent>(view.front());
            check(component.ScriptPath == path, "with its path");
            check(!component.Enabled, "and its enabled flag");
            check(component.RuntimeIndex == 0xFFFFFFFFu, "and no stale runtime slot");
        }
    }

    section("a malformed scene is refused, not thrown");
    {
        const auto write = [](const std::string& name, const std::string& text)
        {
            const std::filesystem::path path = g_ScriptDir / name;
            std::ofstream(path, std::ios::trunc) << text;
            return path.string();
        };
        // A camera missing the fields the deserializer reads with as<T>().
        const std::string missingField = write("missing_field.unhuman", R"(Scene: Untitled
Entities:
  - Entity: 42
    CameraComponent:
      Camera:
        ProjectionType: 0
)");
        const std::string notYaml = write("not_yaml.unhuman", "Scene: [unclosed\n  - {");
        const std::string wrongType = write("wrong_type.unhuman", "Scene: Untitled\nEntities:\n  - Entity: not-a-number\n");

        Ref<Scene> scene = CreateRef<Scene>();
        check(!SceneSerializer(scene).Deserialize(missingField), "a missing component field returns false");
        check(!SceneSerializer(CreateRef<Scene>()).Deserialize(notYaml), "a file that is not YAML returns false");
        check(!SceneSerializer(CreateRef<Scene>()).Deserialize(wrongType), "a mistyped entity id returns false");
        check(!SceneSerializer(CreateRef<Scene>()).Deserialize((g_ScriptDir / "absent.unhuman").string()),
              "a missing file returns false");
    }

    section("editor diagnostics");
    {
        // What the editor's Scripts panel reads: one row per attached script,
        // with the state that tells "running" from "failed", and a per-entity
        // reload for its Reload button.
        const std::string healthy = WriteScript("diag-ok.lua", "function OnUpdate(self, dt) end\n");
        const std::string broken = WriteScript("diag-bad.lua", "function OnUpdate(self, dt) error('nope') end\n");

        Scene scene;
        Entity good = SpawnScripted(scene, "Alpha", healthy);
        Entity bad = SpawnScripted(scene, "Beta", broken);
        Entity plain = scene.CreateEntity("Plain"); // no script at all
        Start(scene);
        Step(scene, 0.1f, 2);

        const std::vector<ScriptEngine::InstanceInfo> infos = engine.Instances(scene);
        check(infos.size() == 2, "one row per attached script");

        auto find = [&infos](const std::string& name) -> const ScriptEngine::InstanceInfo*
        {
            for (const auto& info : infos)
            {
                if (info.EntityName == name)
                {
                    return &info;
                }
            }
            return nullptr;
        };

        const ScriptEngine::InstanceInfo* alpha = find("Alpha");
        const ScriptEngine::InstanceInfo* beta = find("Beta");
        check(alpha != nullptr && beta != nullptr, "both entities are listed by name");
        check(alpha != nullptr && alpha->Started && !alpha->Errored, "the healthy one reads as running");
        check(alpha != nullptr && alpha->Path == healthy, "and carries its path");
        check(beta != nullptr && beta->Errored && !beta->Error.empty(), "the broken one reads as failed");
        check(alpha != nullptr && alpha->ReloadCount == 0, "nothing has reloaded it yet");

        check(engine.ReloadEntity(scene, static_cast<u32>(good)), "reloading one entity reports success");
        const std::vector<ScriptEngine::InstanceInfo> afterReload = engine.Instances(scene);
        for (const auto& info : afterReload)
        {
            if (info.EntityName == "Alpha")
            {
                check(info.ReloadCount == 1, "and the reload is visible to the panel");
            }
        }

        check(!engine.ReloadEntity(scene, static_cast<u32>(plain)), "an entity with no script is not reloaded");

        // Retargeting the component (what "Attach to selected entity" does) gives
        // the entity a fresh instance on the next step.
        bad.GetComponent<ScriptComponent>().ScriptPath = healthy;
        bad.GetComponent<ScriptComponent>().RuntimeIndex = 0xFFFFFFFFu;
        Step(scene, 0.1f, 2);
        for (const auto& info : engine.Instances(scene))
        {
            if (info.EntityName == "Beta")
            {
                check(!info.Errored && info.Started, "a retargeted script starts clean");
            }
        }
    }

    section("teardown");
    {
        Scene scene;
        SpawnScripted(scene, "Teardown", WriteScript("teardown.lua", R"(
function OnCreate(self) self.started = true end
function OnUpdate(self, dt) end
)"));
        Start(scene);
        check(engine.InstanceCount() == 1, "one instance");
        engine.OnSceneStop(scene);
        check(engine.InstanceCount() == 0, "stopping the scene releases it");
    }

    section("an instance that ends while its own hook runs");
    {
        // "Counter" counts OnDestroy calls on its x position: script envs are
        // separate, so a shared global cannot.
        const std::string countDestroy = R"(
function OnDestroy(self) Find("Counter"):Translate(1, 0, 0) end
)";
        Scene scene;
        scene.CreateEntity("Counter");
        SpawnScripted(scene, "SelfDestroyUpdate", WriteScript("self_destroy_update.lua", countDestroy + R"(
function OnUpdate(self, dt) self:Destroy() end
)"));
        SpawnScripted(scene, "SelfDestroyThenError", WriteScript("self_destroy_error.lua", countDestroy + R"(
function OnUpdate(self, dt) self:Destroy(); error("after destroy") end
)"));
        SpawnScripted(scene, "SelfDestroyInOnDestroy", WriteScript("self_destroy_ondestroy.lua", R"(
function OnDestroy(self)
    Find("Counter"):Translate(1, 0, 0)
    self:Destroy()
end
)"));
        Start(scene);
        check(engine.InstanceCount() == 3, "three scripted entities");

        // Under ASan this is the use-after-free regression: the instance whose
        // OnUpdate is running is released from inside that call.
        Step(scene, 0.016f, 2);
        check(!HasEntity(scene, "SelfDestroyUpdate"), "self:Destroy() in OnUpdate destroys the entity");
        check(!HasEntity(scene, "SelfDestroyThenError"), "and so does one that errors afterwards");
        check(std::abs(PositionOf(scene, "Counter").x - 2.0f) < 1e-4f, "each ran OnDestroy exactly once");
        check(engine.InstanceCount() == 1, "both instances are released once the hook returns");

        Entity last = [&]
        {
            auto view = scene.GetRegistry().view<TagComponent>();
            for (const entt::entity handle : view)
            {
                if (view.get<TagComponent>(handle).Tag == "SelfDestroyInOnDestroy")
                {
                    return Entity{handle, &scene};
                }
            }
            return Entity{};
        }();
        scene.DestroyEntity(last);
        check(!HasEntity(scene, "SelfDestroyInOnDestroy"), "an OnDestroy that destroys its own entity ends it");
        check(std::abs(PositionOf(scene, "Counter").x - 3.0f) < 1e-4f, "without running OnDestroy twice");
        check(engine.InstanceCount() == 0, "and its instance is released");
        engine.OnSceneStop(scene);
    }

    section("changing a script's path");
    {
        Scene scene;
        scene.CreateEntity("Counter");
        Entity entity = SpawnScripted(scene, "Retargeted", WriteScript("path_old.lua", R"(
function OnDestroy(self) Find("Counter"):Translate(1, 0, 0) end
)"));
        const std::string next = WriteScript("path_new.lua", R"(
function OnCreate(self) Find("Counter"):Translate(0, 1, 0) end
)");
        Start(scene);
        Step(scene, 0.016f);
        entity.GetComponent<ScriptComponent>().ScriptPath = next;
        Step(scene, 0.016f);
        const glm::vec3 counter = PositionOf(scene, "Counter");
        check(std::abs(counter.x - 1.0f) < 1e-4f, "the old script's OnDestroy runs");
        check(std::abs(counter.y - 1.0f) < 1e-4f, "and the new script starts");
        check(engine.InstanceCount() == 1, "one instance remains");
        engine.OnSceneStop(scene);
    }

    section("bytecode is refused");
    {
        Scene scene;
        // The signature Lua uses to recognize a precompiled chunk.
        SpawnScripted(scene, "Binary", WriteScript("binary.lua", std::string("\x1bLua\x54\x00", 6) + "payload"));
        Start(scene);
        check(engine.ErroredInstanceCount() == 1, "a binary chunk does not load");
        check(engine.LastError().find("binary") != std::string::npos, "and the error says why");
        check(!engine.Run(std::string("\x1bLua\x54\x00", 6), "binary_run"), "Run refuses one too");
        engine.OnSceneStop(scene);
    }

    section("each errored instance keeps its own error");
    {
        Scene scene;
        SpawnScripted(scene, "FailsFirst", WriteScript("fails_first.lua", "function OnCreate(self) error('first') end\n"));
        SpawnScripted(scene, "FailsSecond", WriteScript("fails_second.lua", "function OnCreate(self) error('second') end\n"));
        Start(scene);
        for (const ScriptEngine::InstanceInfo& info : engine.Instances(scene))
        {
            const std::string expected = info.EntityName == "FailsFirst" ? "first" : "second";
            check(info.Errored && info.Error.find(expected) != std::string::npos,
                  info.EntityName + " reports its own error");
        }
        engine.OnSceneStop(scene);
    }

    engine.Shutdown();
    check(!engine.IsInitialized(), "the engine shuts down");
    // Shutdown closes the VM; the static engine dying at exit must not touch
    // it again (under ASan that was a use-after-free in sol's finalizers).
    engine.Init();
    check(engine.Run("assert(math.floor(2.5) == 2)", "reinit"), "it initializes again on a fresh VM");
    engine.Shutdown();

    Physics::PhysicsSystem3D::Shutdown();

    std::filesystem::remove_all(g_ScriptDir, ec);

    std::cout << (g_failures == 0 ? "\nALL CHECKS PASSED\n" : "\nFAILURES PRESENT\n");
    return g_failures == 0 ? 0 : 1;
}
