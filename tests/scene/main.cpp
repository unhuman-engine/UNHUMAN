// Scene-lifetime validation harness (issue #17 follow-up).
//
// Compiles the REAL Scene.cpp, SceneCamera.cpp, UUID.cpp, VfsSystem.cpp and
// PhysicsSystem3D.cpp plus the real box2d and Jolt libraries, and stubs only
// the GPU-facing renderer/font/texture layer (stubs.cpp). No GPU required;
// under the ASan preset this is the regression net for the entt lifetime
// crashes: script instances, parent/child links, deferred mutations, and the
// play-mode scene copy.
#include <cassert>
#include <iostream>
#include "UHE/Core/SimulationClock.h"
#include "UHE/Math/Math.h"
#include "UHE/Physics/PhysicsSystem3D.h"
#include "UHE/Renderer/EditorCamera.h"
#include "UHE/Scene/Components.h"
#include "UHE/Scene/Entity.h"
#include "UHE/Scene/Scene.h"
#include "UHE/Scene/ScriptableEntity.h"

using namespace UHE;

static int g_failures = 0;

#define CHECK(cond, msg)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (cond)                                                                                  \
            std::cout << "  ok  - " << msg << '\n';                                                \
        else                                                                                       \
        {                                                                                          \
            std::cout << "  FAIL - " << msg << '\n';                                               \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

// ---- script lifecycle counters ----
static int s_LiveInstances = 0;
static int s_OnCreateCalls = 0;
static int s_OnDestroyCalls = 0;
static int s_OnUpdateCalls = 0;

class TestScript : public ScriptableEntity
{
    void OnCreate() override { ++s_OnCreateCalls; ++s_LiveInstances; }
    void OnDestroy() override { --s_LiveInstances; ++s_OnDestroyCalls; }
    void OnUpdate(Timestep) override { ++s_OnUpdateCalls; }
};

static void ResetCounters()
{
    s_LiveInstances = s_OnCreateCalls = s_OnDestroyCalls = s_OnUpdateCalls = 0;
}

// Shared headless EditorCamera used to drive OnUpdateEditor frames.
static EditorCamera& cam()
{
    static EditorCamera c(45.0f, 1.766f, 0.1f, 1000.0f);
    return c;
}

// --- 1. entity creation + UUID index ---
static void TestCreationAndIndex()
{
    std::cout << "[creation + uuid index]\n";
    auto scene = CreateRef<Scene>();

    Entity a = scene->CreateEntity("A");
    Entity b = scene->CreateEntity("B");
    CHECK(a && b, "entities created valid");
    CHECK(a != b, "entity handles distinct");
    CHECK(a.GetUUID() != 0 && a.GetUUID() != b.GetUUID(), "UUIDs unique and nonzero");
    CHECK(scene->GetEntityWithUUID(a.GetUUID()) == a, "UUID index resolves A");
    CHECK(scene->GetEntityWithUUID(b.GetUUID()) == b, "UUID index resolves B");
    CHECK(!scene->GetEntityWithUUID(0xDEADBEEF), "unknown UUID yields empty entity");

    CHECK(scene->GetRootEntities().size() == 2, "both entities are roots");
}

// --- 2. deferred child creation + reparent + world transforms ---
static void TestHierarchy()
{
    std::cout << "[hierarchy]\n";
    auto scene = CreateRef<Scene>();
    EditorCamera cam;

    Entity parent = scene->CreateEntity("Parent");
    parent.GetComponent<TransformComponent>().Translation = {10.0f, 0.0f, 0.0f};

    // Deferred path (used by the editor UI): attach happens on next update.
    Entity child = scene->CreateChildEntity(parent, "Child");
    scene->OnUpdateEditor(Timestep(0.016f), cam); // flush pending mutations

    CHECK(scene->GetParentEntity(child) == parent, "deferred CreateChildEntity attached");
    CHECK(parent.GetUUID() == child.GetComponent<RelationshipComponent>().Parent, "parent UUID stored on child");
    CHECK(scene->GetRootEntities().size() == 1, "child not listed as root");

    // Child world transform must include the parent's.
    glm::mat4 childWorld = scene->GetWorldSpaceTransformMatrix(child);
    glm::vec3 translation, rotation, scale;
    Math::DecomposeTransform(childWorld, translation, rotation, scale);
    CHECK(translation.x > 9.999f && translation.x < 10.001f, "child world inherits parent translation");

    // Reparent to a new parent, world position must be preserved.
    Entity parent2 = scene->CreateEntity("Parent2");
    parent2.GetComponent<TransformComponent>().Translation = {0.0f, 5.0f, 0.0f};
    scene->ReparentEntity(child, parent2);
    scene->OnUpdateEditor(Timestep(0.016f), cam);

    childWorld = scene->GetWorldSpaceTransformMatrix(child);
    Math::DecomposeTransform(childWorld, translation, rotation, scale);
    CHECK(translation.x > 9.999f && translation.x < 10.001f, "reparent keeps world X");
    CHECK(translation.y > -0.001f && translation.y < 0.001f, "reparent keeps world Y (unchanged)");

    // Cycle rejection: parenting Parent2 under its own child must be refused.
    scene->ReparentEntity(parent2, child);
    CHECK(scene->GetParentEntity(parent2) == Entity{}, "cycle reparent rejected");

    // CollapseEntity detaches ONLY the entity; children stay attached to it.
    Entity grandchild = scene->CreateChildEntity(child, "Grandchild");
    scene->OnUpdateEditor(Timestep(0.016f), cam);
    scene->CollapseEntity(child);
    scene->OnUpdateEditor(Timestep(0.016f), cam);

    CHECK(scene->GetParentEntity(child) == Entity{}, "collapsed entity is a root");
    CHECK(scene->GetParentEntity(grandchild) == child, "collapse keeps children attached");
    scene->OnUpdateEditor(Timestep(0.016f), cam);
    glm::mat4 gcWorld = scene->GetWorldSpaceTransformMatrix(grandchild);
    Math::DecomposeTransform(gcWorld, translation, rotation, scale);
    CHECK(translation.x > 9.999f && translation.x < 10.001f, "subtree moved with collapsed entity");
}

// --- 3. recursive destroy + selection safety ---
static void TestRecursiveDestroy()
{
    std::cout << "[recursive destroy]\n";
    auto scene = CreateRef<Scene>();

    Entity root = scene->CreateEntity("Root");
    Entity mid = scene->CreateChildEntity(root, "Mid");
    Entity leaf = scene->CreateChildEntity(mid, "Leaf");
    scene->OnUpdateEditor(Timestep(0.016f), cam());

    u64 leafID = leaf.GetUUID();
    scene->DestroyEntity(root);

    CHECK(!scene->GetEntityWithUUID(leafID), "destroy removed the whole subtree from the UUID index");
    CHECK(scene->GetRootEntities().empty(), "no roots left after subtree destroy");
}

// --- 4. script instance lifetime (the entt crash class) ---
static void TestScriptLifetime()
{
    std::cout << "[script lifetime]\n";
    ResetCounters();
    {
        auto scene = CreateRef<Scene>();
        Entity e = scene->CreateEntity("Scripted");
        e.AddComponent<NativeScriptComponent>().Bind<TestScript>();

        scene->OnUpdateEditor(Timestep(0.016f), cam());
        CHECK(s_OnCreateCalls == 1, "OnCreate ran once on first update");
        CHECK(s_LiveInstances == 1, "instance alive after instantiate");

        scene->OnUpdateEditor(Timestep(0.016f), cam());
        CHECK(s_OnCreateCalls == 1, "instance not re-instantiated");
        CHECK(s_OnUpdateCalls == 2, "OnUpdate ran each frame");

        // Deleting the entity must destroy the instance (ASan verifies the
        // delete; the counter verifies OnDestroy ran while components lived).
        scene->DestroyEntity(e);
        CHECK(s_OnDestroyCalls == 1, "OnDestroy ran before handle died");
        CHECK(s_LiveInstances == 0, "no leaked instances after DestroyEntity");
    }
    {
        // Scene destruction with a live instance must also release it.
        ResetCounters();
        auto scene = CreateRef<Scene>();
        Entity e = scene->CreateEntity("Scripted");
        e.AddComponent<NativeScriptComponent>().Bind<TestScript>();
        scene->OnUpdateEditor(Timestep(0.016f), cam());
        CHECK(s_LiveInstances == 1, "instance alive before scene teardown");
    }
    CHECK(s_LiveInstances == 0, "~Scene destroyed remaining instances");

    {
        // Play-mode copy: runtime scene owns its own instances and
        // OnRuntimeStop must release them (they were leaked before).
        ResetCounters();
        auto editorScene = CreateRef<Scene>();
        Entity e = editorScene->CreateEntity("Scripted");
        e.AddComponent<NativeScriptComponent>().Bind<TestScript>();

        Ref<Scene> runtimeScene = Scene::Copy(editorScene);
        runtimeScene->OnRuntimeStart();
        runtimeScene->OnUpdateRuntime(Timestep(0.016f));
        CHECK(s_OnCreateCalls == 1, "runtime copy instantiated its own script");
        CHECK(s_LiveInstances == 1, "runtime instance alive during play");

        runtimeScene->OnRuntimeStop();
        CHECK(s_LiveInstances == 0, "OnRuntimeStop released runtime instances");
        CHECK(s_OnDestroyCalls == 1, "OnDestroy ran on runtime stop");
    }
    ResetCounters();
}

// --- 5. real 2D physics runtime (box2d world create/step/destroy) ---
static void TestRuntimePhysics()
{
    std::cout << "[runtime physics]\n";
    auto scene = CreateRef<Scene>();

    Entity body = scene->CreateEntity("FallingBox");
    auto& rb2d = body.AddComponent<RigidBody2DComponent>();
    rb2d.Type = RigidBody2DComponent::BodyType::Dynamic;
    body.AddComponent<BoxColliderComponent>();

    Entity floor = scene->CreateEntity("Floor");
    floor.GetComponent<TransformComponent>().Translation = {0.0f, -10.0f, 0.0f};
    floor.AddComponent<RigidBody2DComponent>(); // static by default
    floor.AddComponent<BoxColliderComponent>();

    scene->OnRuntimeStart();
    float yBefore = body.GetComponent<TransformComponent>().Translation.y;
    for (int i = 0; i < 30; i++)
        scene->OnUpdateRuntime(Timestep(1.0f / 60.0f));
    float yAfter = body.GetComponent<TransformComponent>().Translation.y;

    CHECK(yAfter < yBefore - 0.5f, "dynamic body fell under gravity (box2d world alive)");
    CHECK(!scene->GetRootEntities().empty(), "hierarchy intact across runtime frames");

    scene->OnRuntimeStop();
    // ASan: world destroyed, no leaks from body/shape user data.
}

// --- 6. relationship sanitization on deserialize-shaped data ---
static void TestStaleRelationships()
{
    std::cout << "[stale relationship repair]\n";
    auto scene = CreateRef<Scene>();
    Entity a = scene->CreateEntity("A");
    Entity b = scene->CreateEntity("B");

    // Corrupt state: A lists itself and B twice, B points at a dead parent.
    auto& relA = a.GetComponent<RelationshipComponent>();
    relA.Children.push_back(a.GetUUID());
    relA.Children.push_back(b.GetUUID());
    relA.Children.push_back(b.GetUUID());
    b.GetComponent<RelationshipComponent>().Parent = 0x1234567890ABCDEF;

    // The panel's root list must not duplicate or ghost anything.
    size_t roots = scene->GetRootEntities().size();
    CHECK(roots == 2, "root list dedupes stale child entries");

    scene->CollapseEntity(a); // self-parent repair path
    CHECK(scene->GetRootEntities().size() == 2, "self-parent repaired to root");
}

// --- 7. issue #49: the fixed-step clock ---
static void TestSimulationClock()
{
    std::cout << "[simulation clock]\n";
    SimulationClock clock; // 120 Hz, at most 30 ticks per advance
    CHECK(clock.GetFixedStepNanoseconds() == 8'333'333, "120 Hz is 8333333 ns per tick");
    CHECK(clock.Advance(1.0 / 60.0) == 2, "a 60 FPS frame runs two ticks");
    CHECK(clock.Advance(1.0 / 240.0) == 0, "half a tick runs none");
    CHECK(clock.Advance(1.0 / 240.0) == 1, "and the remainder carries into the next frame");

    SimulationClock counted;
    u64 ticks = 0;
    for (int i = 0; i < 1000; ++i)
        ticks += counted.Advance(1.0 / 144.0);
    // 1000 frames of llround(1e9 / 144) = 6944444 ns, over 8333333 ns ticks.
    CHECK(ticks == 6'944'444'000ull / 8'333'333ull, "1000 frames at 144 FPS give exactly the integer tick count");

    SimulationClock capped;
    CHECK(capped.Advance(10.0) == SimulationClock::kDefaultMaxTicksPerAdvance, "a 10 s hitch is capped");
    CHECK(capped.Alpha() >= 0.0f && capped.Alpha() < 1.0f, "and keeps less than one tick of backlog");

    SimulationClock paused;
    paused.SetPaused(true);
    CHECK(paused.Advance(1.0) == 0, "a paused clock runs no ticks");
    paused.SetPaused(false);
    CHECK(paused.Advance(1.0 / 240.0) == 0 && paused.Alpha() > 0.49f && paused.Alpha() < 0.51f,
          "and accumulated nothing while paused");

    SimulationClock indexed;
    indexed.OnTickCompleted();
    indexed.OnTickCompleted();
    CHECK(indexed.GetTickIndex() == 2, "the tick index counts completed ticks");
    indexed.Reset();
    CHECK(indexed.GetTickIndex() == 0 && indexed.Alpha() == 0.0f, "Reset returns to tick 0");
}

// A box falling onto a floor in both physics engines.
static Ref<Scene> MakeFallingScene()
{
    auto scene = CreateRef<Scene>();
    Entity box3d = scene->CreateEntity("Box3D");
    box3d.GetComponent<TransformComponent>().Translation = {0.3f, 5.0f, -0.2f};
    box3d.GetComponent<TransformComponent>().Rotation = {0.2f, 0.4f, 0.1f};
    box3d.AddComponent<RigidBody3DComponent>().Type = RigidBody3DComponent::BodyType::Dynamic;
    box3d.AddComponent<BoxCollider3DComponent>();

    Entity floor3d = scene->CreateEntity("Floor3D");
    floor3d.GetComponent<TransformComponent>().Scale = {20.0f, 1.0f, 20.0f};
    floor3d.AddComponent<RigidBody3DComponent>(); // static
    floor3d.AddComponent<BoxCollider3DComponent>();

    Entity box2d = scene->CreateEntity("Box2D");
    box2d.GetComponent<TransformComponent>().Translation = {0.0f, 4.0f, 0.0f};
    box2d.AddComponent<RigidBody2DComponent>().Type = RigidBody2DComponent::BodyType::Dynamic;
    box2d.AddComponent<BoxColliderComponent>();

    Entity floor2d = scene->CreateEntity("Floor2D");
    floor2d.GetComponent<TransformComponent>().Translation = {0.0f, -2.0f, 0.0f};
    floor2d.AddComponent<RigidBody2DComponent>();
    floor2d.AddComponent<BoxColliderComponent>();
    return scene;
}

static TransformComponent PoseOf(Scene& scene, const std::string& name)
{
    auto view = scene.GetRegistry().view<TagComponent, TransformComponent>();
    for (auto entity : view)
        if (view.get<TagComponent>(entity).Tag == name)
            return view.get<TransformComponent>(entity);
    return {};
}

static bool SamePose(const TransformComponent& a, const TransformComponent& b)
{
    return a.Translation == b.Translation && a.Rotation == b.Rotation; // bit-exact
}

// --- 8. issue #49 acceptance: frame rate does not change the simulation ---
static void TestFixedStepDeterminism()
{
    std::cout << "[fixed-step determinism]\n";
    constexpr u64 kTicks = 240; // two simulated seconds

    auto runAt = [&](double fps)
    {
        Ref<Scene> scene = MakeFallingScene();
        scene->OnRuntimeStart();
        while (scene->GetSimulationClock().GetTickIndex() < kTicks)
            scene->OnUpdateRuntimeSimulation(Timestep(static_cast<float>(1.0 / fps)));
        return scene;
    };

    Ref<Scene> slow = runAt(30.0);
    Ref<Scene> fast = runAt(144.0);
    Ref<Scene> stepped = MakeFallingScene();
    stepped->OnRuntimeStart();
    for (u64 i = 0; i < kTicks; ++i)
        stepped->StepSimulation();

    CHECK(slow->GetSimulationClock().GetTickIndex() == kTicks && fast->GetSimulationClock().GetTickIndex() == kTicks,
          "30 FPS and 144 FPS both land on tick 240");
    CHECK(PoseOf(*slow, "Box3D").Translation.y < 4.0f, "the 3D box fell");
    CHECK(PoseOf(*slow, "Box2D").Translation.y < 3.0f, "the 2D box fell");
    CHECK(SamePose(PoseOf(*slow, "Box3D"), PoseOf(*fast, "Box3D")), "3D pose is identical at 30 and 144 FPS");
    CHECK(SamePose(PoseOf(*slow, "Box2D"), PoseOf(*fast, "Box2D")), "2D pose is identical at 30 and 144 FPS");
    CHECK(SamePose(PoseOf(*slow, "Box3D"), PoseOf(*stepped, "Box3D")) &&
              SamePose(PoseOf(*slow, "Box2D"), PoseOf(*stepped, "Box2D")),
          "and identical to 240 manual single steps");

    // Pause: frames keep coming, the simulation does not move.
    const TransformComponent before = PoseOf(*fast, "Box3D");
    fast->GetSimulationClock().SetPaused(true);
    for (int i = 0; i < 30; ++i)
        fast->OnUpdateRuntimeSimulation(Timestep(1.0f / 60.0f));
    CHECK(fast->GetSimulationClock().GetTickIndex() == kTicks && SamePose(before, PoseOf(*fast, "Box3D")),
          "a paused clock freezes the simulation while frames continue");

    slow->OnRuntimeStop();
    fast->OnRuntimeStop();
    stepped->OnRuntimeStop();
}

int main()
{
    Scene::GetEditorOverlays().LightIcons = false; // no texture loading in headless tests
    Physics::PhysicsSystem3D::Init();   // Jolt global registration (Factory/RegisterTypes)

    TestCreationAndIndex();
    TestHierarchy();
    TestRecursiveDestroy();
    TestScriptLifetime();
    TestRuntimePhysics();
    TestStaleRelationships();
    TestSimulationClock();
    TestFixedStepDeterminism();

    Physics::PhysicsSystem3D::Shutdown();

    if (g_failures == 0)
    {
        std::cout << "\nALL SCENE TESTS PASSED\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " SCENE TEST(S) FAILED\n";
    return 1;
}
