// Scene-lifetime validation harness (issue #17 follow-up).
//
// Compiles the REAL Scene.cpp, SceneCamera.cpp, UUID.cpp, VfsSystem.cpp and
// PhysicsSystem3D.cpp plus the real box2d and Jolt libraries, and stubs only
// the GPU-facing renderer/font/texture layer (stubs.cpp). No GPU required;
// under the ASan preset this is the regression net for the entt lifetime
// crashes: script instances, parent/child links, deferred mutations, and the
// play-mode scene copy.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "UHE/Core/SimulationClock.h"
#include "UHE/Math/Math.h"
#include "UHE/Physics/PhysicsSystem3D.h"
#include "UHE/Renderer/EditorCamera.h"
#include "UHE/Scene/Components.h"
#include "UHE/Scene/Entity.h"
#include "UHE/Scene/Scene.h"
#include "UHE/Scene/ScriptableEntity.h"
#include "UHE/Scene/Terrain.h"

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

// The scene camera's clip planes, as the serializer and the inspector read them.
static void TestSceneCameraClipPlanes()
{
    SceneCamera camera;
    camera.SetViewportSize(1600, 900);
    camera.SetPerspective(glm::radians(50.0f), 0.1f, 500.0f);
    camera.SetOrthographic(10.0f, -1.0f, 1.0f);
    camera.SetProjectionType(SceneCamera::ProjectionType::Perpective);
    // GetProjectionFar returned the orthographic far plane (1), which every
    // saved scene then loaded as its perspective far plane.
    CHECK(camera.GetProjectionFar() == 500.0f, "perspective far plane is its own, not the orthographic one");
    CHECK(camera.GetProjectionNear() == 0.1f, "perspective near plane round-trips");
    CHECK(camera.GetOrthographicFarClip() == 1.0f, "orthographic far plane is unchanged");

    camera.SetProjectionType(SceneCamera::ProjectionType::Orhtographic);
    const glm::mat4 before = camera.GetProjection();
    camera.SetOrthoGraphicNearClip(-5.0f);
    CHECK(camera.GetOrthographicNearClip() == -5.0f, "orthographic near plane is set");
    CHECK(camera.GetProjection() != before, "changing the orthographic near plane updates the projection");

    // Scenes saved with the old getter load with a usable far plane.
    bool repaired = false;
    CHECK(SceneCamera::LoadedPerspectiveFar(0.01f, 1.0f, 1.0f, repaired) == SceneCamera::kDefaultPerspectiveFar &&
              repaired,
          "a far plane equal to the orthographic one (the old bug) is repaired");
    CHECK(SceneCamera::LoadedPerspectiveFar(0.5f, 0.2f, 1.0f, repaired) == SceneCamera::kDefaultPerspectiveFar &&
              repaired,
          "a far plane before the near plane is repaired");
    CHECK(SceneCamera::LoadedPerspectiveFar(0.1f, 500.0f, 1.0f, repaired) == 500.0f && !repaired,
          "a real far plane is kept");
}

// --- terrain: heights, mesh, and a body that things land on ---
static void TestTerrain()
{
    std::cout << "[terrain]\n";
    TerrainComponent terrain;
    terrain.Size = 32.0f;
    terrain.Height = 6.0f;
    terrain.Resolution = 32;

    const Terrain::Heightfield a = Terrain::BuildHeightfield(terrain);
    const Terrain::Heightfield b = Terrain::BuildHeightfield(terrain);
    CHECK(a.Samples == 33 && a.Heights.size() == 33u * 33u, "a 32-quad grid has 33 x 33 samples");
    CHECK(a.Heights == b.Heights, "the same seed builds the same ground");
    const auto [low, high] = std::minmax_element(a.Heights.begin(), a.Heights.end());
    CHECK(std::fabs(*low) < 1e-4f && std::fabs(*high - 6.0f) < 1e-3f, "noise spans 0 to Height");
    terrain.Seed = 2;
    CHECK(Terrain::BuildHeightfield(terrain).Heights != a.Heights, "another seed builds other ground");
    terrain.Seed = 1;

    // HeightAt hits the samples exactly, and interpolates between them.
    const float step = a.Size / 32.0f;
    const float corner = Terrain::HeightAt(a, -16.0f, -16.0f);
    CHECK(std::fabs(corner - a.Heights[0]) < 1e-5f, "HeightAt reads the corner sample");
    const float mid = Terrain::HeightAt(a, -16.0f + step * 0.5f, -16.0f);
    CHECK(std::fabs(mid - (a.Heights[0] + a.Heights[1]) * 0.5f) < 1e-4f, "HeightAt interpolates between samples");
    CHECK(std::fabs(Terrain::HeightAt(a, -100.0f, -100.0f) - corner) < 1e-5f, "outside the edge clamps");

    const RD3d::Primitive prim = Terrain::BuildPrimitive(a, terrain);
    CHECK(prim.vertices.size() == 33u * 33u && prim.indices.size() == 32u * 32u * 6u, "one vertex per sample");
    bool upward = true;
    for (const auto& v : prim.vertices)
        upward &= v.normal.y > 0.0f && std::fabs(glm::length(v.normal) - 1.0f) < 1e-4f;
    CHECK(upward, "normals are unit length and face up");
    // Triangle winding faces up (counter-clockwise from above), like the normals.
    const glm::vec3 p0 = prim.vertices[prim.indices[0]].position;
    const glm::vec3 p1 = prim.vertices[prim.indices[1]].position;
    const glm::vec3 p2 = prim.vertices[prim.indices[2]].position;
    CHECK(glm::cross(p1 - p0, p2 - p0).y > 0.0f, "triangles are front-facing from above");

    // A flat field: grass everywhere, normals straight up; a cliff is rock.
    Terrain::Heightfield flat;
    flat.Samples = 9;
    flat.Size = 8.0f;
    flat.Heights.assign(81, 3.0f);
    const RD3d::Primitive flatPrim = Terrain::BuildPrimitive(flat, terrain);
    CHECK(glm::length(flatPrim.vertices[40].normal - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-5f, "flat ground faces up");
    Terrain::Heightfield cliff = flat;
    for (u32 z = 0; z < 9; ++z)
        for (u32 x = 5; x < 9; ++x)
            cliff.Heights[z * 9 + x] = 6.0f; // a 3 m step over one metre
    const RD3d::Primitive cliffPrim = Terrain::BuildPrimitive(cliff, terrain);
    const glm::vec3 rock = glm::vec3(cliffPrim.vertices[4 * 9 + 4].color);
    const glm::vec3 grass = glm::vec3(flatPrim.vertices[40].color);
    CHECK(rock.g < grass.g * 0.8f, "steep ground takes the rock colour");

    // A heightmap image: white is Height, black is 0, rows run along +Z.
    const std::filesystem::path map = std::filesystem::temp_directory_path() / "uhe_terrain_map.pgm";
    {
        std::ofstream file(map, std::ios::binary);
        file << "P5\n4 4\n255\n";
        for (int z = 0; z < 4; ++z)
            for (int x = 0; x < 4; ++x)
                file.put(static_cast<char>(z == 0 ? 255 : 0));
    }
    terrain.Resolution = 8;
    const Terrain::Heightfield image = Terrain::BuildHeightfield(terrain, map.string());
    CHECK(std::fabs(Terrain::HeightAt(image, 0.0f, -16.0f) - 6.0f) < 1e-3f, "the image's top row is the -Z edge");
    CHECK(std::fabs(Terrain::HeightAt(image, 0.0f, 16.0f)) < 1e-3f, "and its bottom row the +Z edge");
    std::filesystem::remove(map);
    const Terrain::Heightfield missing = Terrain::BuildHeightfield(terrain, "/no/such/heightmap.png");
    CHECK(missing.Samples == 9 && !missing.Heights.empty(), "a missing heightmap falls back to noise");

    CHECK(Terrain::Resample(a, 34).Samples == 34, "resampling gives the asked sample count");

    // In a scene: built on the first update, rebuilt only when a field changes,
    // and solid ground in Play.
    auto scene = CreateRef<Scene>();
    Entity ground = scene->CreateEntity("Ground");
    TerrainComponent& tc = ground.AddComponent<TerrainComponent>();
    tc.Size = 40.0f;
    tc.Height = 4.0f;
    tc.Resolution = 64;
    Entity ball = scene->CreateEntity("Ball");
    ball.GetComponent<TransformComponent>().Translation = {3.0f, 10.0f, -2.0f};
    auto& rb = ball.AddComponent<RigidBody3DComponent>();
    rb.Type = RigidBody3DComponent::BodyType::Dynamic;
    ball.AddComponent<SphereCollider3DComponent>().Radius = 0.5f;

    scene->OnUpdateEditorSimulation(Timestep(0.016f));
    CHECK(tc.Mesh != nullptr && tc.Heights != nullptr && tc.Heights->size() == 65u * 65u, "built on first update");
    const Ref<RD3d::Model> firstMesh = tc.Mesh;
    scene->OnUpdateEditorSimulation(Timestep(0.016f));
    CHECK(tc.Mesh == firstMesh, "not rebuilt while nothing changed");
    tc.Height = 5.0f;
    scene->OnUpdateEditorSimulation(Timestep(0.016f));
    CHECK(tc.Mesh != firstMesh, "rebuilt when a field changes");

    Terrain::Heightfield built;
    built.Samples = 65;
    built.Size = 40.0f;
    built.Heights = *tc.Heights;

    scene->OnRuntimeStart();
    for (int i = 0; i < 240; ++i)
        scene->OnUpdateRuntime(Timestep(1.0f / 60.0f));
    // The ball rolls downhill: compare with the ground where it ended up.
    const glm::vec3 at = ball.GetComponent<TransformComponent>().Translation;
    const float groundY = Terrain::HeightAt(built, at.x, at.z);
    std::cout << "  ball at (" << at.x << ", " << at.y << ", " << at.z << "), ground there " << groundY << " m\n";
    CHECK(at.y > groundY - 0.1f, "the ball does not fall through the terrain");
    CHECK(at.y < groundY + 1.0f, "it rests on the ground, not above it");
    CHECK(at.y < 9.0f, "the ball fell from where it started");
    scene->OnRuntimeStop();
}

int main()
{
    Scene::GetEditorOverlays().LightIcons = false; // no texture loading in headless tests
    Physics::PhysicsSystem3D::Init();   // Jolt global registration (Factory/RegisterTypes)

    TestCreationAndIndex();
    TestSceneCameraClipPlanes();
    TestTerrain();
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
