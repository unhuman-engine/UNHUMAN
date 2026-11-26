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
#include "UHE/Physics/Joints3D.h"
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

// --- an Entity kept across frames: the scene or the entity may be gone ---
static void TestStaleEntityHandles()
{
    std::cout << "[stale entity handles]\n";
    Entity kept;
    u64 deadSceneId = 0;
    {
        auto scene = CreateRef<Scene>();
        kept = scene->CreateEntity("Hovered");
        deadSceneId = scene->GetInstanceId();
        CHECK(kept && kept.IsValid() && kept.HasComponent<TagComponent>(), "a live entity is valid");
        CHECK(kept.TryGetComponent<TagComponent>() != nullptr && kept.TryGetComponent<CameraComponent>() == nullptr,
              "TryGetComponent returns the component it has, null for one it lacks");
        CHECK(Scene::IsAlive(deadSceneId), "the scene is alive while it exists");
    } // the editor's Play/Stop: the scene the handle came from is destroyed
    CHECK(!Scene::IsAlive(deadSceneId), "a destroyed scene is not alive");
    // Each of these read the freed registry and crashed the status bar.
    CHECK(!kept && !kept.IsValid(), "a handle into a destroyed scene is invalid");
    CHECK(!kept.HasComponent<TagComponent>(), "and has no components");
    CHECK(kept.TryGetComponent<TagComponent>() == nullptr && kept.GetScene() == nullptr,
          "and gives no component and no scene");

    // A new scene at, possibly, the same address does not revive the old handle.
    auto next = CreateRef<Scene>();
    next->CreateEntity("Other");
    CHECK(next->GetInstanceId() != deadSceneId && !kept, "scene ids are never reused");

    // The entity destroyed, the scene alive.
    Entity doomed = next->CreateEntity("Doomed");
    Entity copy = doomed;
    next->DestroyEntity(doomed);
    CHECK(!copy && copy.TryGetComponent<TagComponent>() == nullptr, "a destroyed entity is invalid");
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
    CHECK(a.Samples == 33 && a.Heights.size() == std::size_t{33} * 33, "a 32-quad grid has 33 x 33 samples");
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
    CHECK(prim.vertices.size() == std::size_t{33} * 33 && prim.indices.size() == std::size_t{32} * 32 * 6,
          "one vertex per sample");
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

    // A rectangular heightmap is stretched, not cropped: an 8 x 4 image whose
    // right half is a plateau keeps the plateau on the +X side.
    const std::filesystem::path wide = std::filesystem::temp_directory_path() / "uhe_terrain_wide.pgm";
    {
        std::ofstream file(wide, std::ios::binary);
        file << "P5\n8 4\n255\n";
        for (int z = 0; z < 4; ++z)
            for (int x = 0; x < 8; ++x)
                file.put(static_cast<char>(x >= 4 ? 255 : 0));
    }
    const Terrain::Heightfield wideField = Terrain::BuildHeightfield(terrain, wide.string());
    CHECK(std::fabs(Terrain::HeightAt(wideField, 15.0f, 0.0f) - terrain.Height) < 1e-3f &&
              std::fabs(Terrain::HeightAt(wideField, -15.0f, 0.0f)) < 1e-3f,
          "a rectangular heightmap keeps its right half");
    std::filesystem::remove(wide);

    // A 45 degree ramp: the same slope at the edges as in the middle.
    Terrain::Heightfield ramp;
    ramp.Samples = 9;
    ramp.Size = 8.0f;
    for (u32 z = 0; z < 9; ++z)
        for (u32 x = 0; x < 9; ++x)
            ramp.Heights.push_back(static_cast<float>(x));
    const RD3d::Primitive rampPrim = Terrain::BuildPrimitive(ramp, terrain);
    const glm::vec3 edgeNormal = rampPrim.vertices[4 * 9 + 0].normal;
    const glm::vec3 middleNormal = rampPrim.vertices[4 * 9 + 4].normal;
    CHECK(glm::length(edgeNormal - middleNormal) < 1e-4f, "edge normals use the one cell they span");

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
    CHECK(tc.Mesh != nullptr && tc.Heights != nullptr && tc.Heights->size() == std::size_t{65} * 65,
          "built on first update");
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

    // The terrain destroyed in Play takes its collider with it.
    scene->DestroyEntity(ground);
    for (int i = 0; i < 120; ++i)
        scene->OnUpdateRuntime(Timestep(1.0f / 60.0f));
    CHECK(ball.GetComponent<TransformComponent>().Translation.y < groundY - 2.0f,
          "a destroyed terrain's collider is removed: the ball falls through");
    scene->OnRuntimeStop();
}

// --- game UI: layout, clicks and sliders ---
/// Joints, motors and sensors for robot simulation (issue #69), checked against physics.
static Entity MakeBody(Ref<Scene>& scene, const char* name, glm::vec3 at, float radius, bool dynamic = true)
{
    Entity e = scene->CreateEntity(name);
    e.GetComponent<TransformComponent>().Translation = at;
    auto& rb = e.AddComponent<RigidBody3DComponent>();
    rb.Type = dynamic ? RigidBody3DComponent::BodyType::Dynamic : RigidBody3DComponent::BodyType::Static;
    rb.Mass = 1.0f;
    rb.LinearDamping = 0.0f;
    rb.AngularDamping = 0.0f;
    e.AddComponent<SphereCollider3DComponent>().Radius = radius;
    return e;
}

static void TestRobotJoints()
{
    std::cout << "TestRobotJoints\n";
    constexpr float kRate = 240.0f;

    // A pendulum: a small ball on a 1 m hinge to the world, released at 0.2 rad.
    // Small-angle period 2*pi*sqrt(L/g) = 2.006 s; the encoder must see it.
    const auto runPendulum = [&](std::vector<float>* angles) -> glm::vec3
    {
        auto scene = CreateRef<Scene>();
        scene->GetPhysicsSettings().TickRate = kRate;
        const float theta = 0.2f;
        Entity bob = MakeBody(scene, "Bob", {std::sin(theta), -std::cos(theta), 0.0f}, 0.02f);
        auto& hinge = bob.AddComponent<Joint3DComponent>();
        hinge.Type = Joint3DComponent::JointType::Hinge;
        hinge.Anchor = -bob.GetComponent<TransformComponent>().Translation; // the world origin, in the bob's space
        hinge.Axis = {0.0f, 0.0f, 1.0f};
        scene->OnRuntimeStart();
        for (int i = 0; i < static_cast<int>(kRate * 6.0f); ++i)
        {
            scene->StepSimulation();
            if (angles != nullptr)
                angles->push_back(bob.GetComponent<Joint3DComponent>().RuntimePosition);
        }
        const glm::vec3 end = bob.GetComponent<TransformComponent>().Translation;
        scene->OnRuntimeStop();
        return end;
    };
    std::vector<float> angles;
    const glm::vec3 endA = runPendulum(&angles);
    // The encoder reads from the pose at Play start (0 at 0.2 rad off vertical),
    // so the swing is around -0.2: count crossings of its mean.
    float mean = 0.0f;
    for (const float a : angles)
        mean += a / static_cast<float>(angles.size());
    std::vector<int> crossings; // ticks where the angle crosses its mean
    for (std::size_t i = 1; i < angles.size(); ++i)
        if ((angles[i - 1] < mean) != (angles[i] < mean))
            crossings.push_back(static_cast<int>(i));
    const float period = crossings.size() >= 5 ? static_cast<float>(crossings[4] - crossings[0]) / 2.0f / kRate
                                               : 0.0f; // four half periods
    const float expected = 2.0f * 3.14159265f * std::sqrt(1.0f / 9.81f);
    std::cout << "  pendulum period " << period << " s, expected " << expected << " s\n";
    CHECK(std::abs(period - expected) < 0.02f * expected, "a hinged pendulum swings with T = 2 pi sqrt(L/g) (2%)");
    float amplitude = 0.0f;
    for (std::size_t i = angles.size() - static_cast<std::size_t>(kRate * 2.0f); i < angles.size(); ++i)
        amplitude = std::max(amplitude, std::abs(angles[i] - mean));
    CHECK(std::abs(mean + 0.2f) < 0.01f && amplitude > 0.18f && amplitude < 0.22f,
          "no damping: it still swings 0.2 rad either side of vertical after 6 s");

    const glm::vec3 endB = runPendulum(nullptr);
    CHECK(endA == endB, "two runs end bit-for-bit at the same place (deterministic)");

    // A wheel on a vertical axle: a velocity motor spins it up to 3 rad/s.
    {
        auto scene = CreateRef<Scene>();
        scene->GetPhysicsSettings().TickRate = kRate;
        scene->GetPhysicsSettings().Gravity = {0.0f, 0.0f, 0.0f};
        Entity wheel = MakeBody(scene, "Wheel", {0.0f, 0.0f, 0.0f}, 0.3f);
        auto& axle = wheel.AddComponent<Joint3DComponent>();
        axle.Type = Joint3DComponent::JointType::Hinge;
        axle.Axis = {0.0f, 1.0f, 0.0f};
        axle.Motor = Joint3DComponent::MotorMode::Velocity;
        axle.MotorTarget = 3.0f;
        axle.MotorMaxForce = 50.0f;
        scene->OnRuntimeStart();
        for (int i = 0; i < static_cast<int>(kRate); ++i)
            scene->StepSimulation();
        const float speed = wheel.GetComponent<Joint3DComponent>().RuntimeVelocity;
        std::cout << "  wheel speed " << speed << " rad/s\n";
        CHECK(std::abs(speed - 3.0f) < 0.05f, "a velocity motor holds its target speed (encoder rate)");

        scene->OnRuntimeStop();
    }
    // Limits: a hinge limited to +-0.5 rad, driven hard past it, stays at the stop.
    {
        auto scene = CreateRef<Scene>();
        scene->GetPhysicsSettings().TickRate = kRate;
        scene->GetPhysicsSettings().Gravity = {0.0f, 0.0f, 0.0f};
        Entity arm = MakeBody(scene, "Arm", {0.0f, 0.0f, 0.0f}, 0.1f);
        auto& joint = arm.AddComponent<Joint3DComponent>();
        joint.Type = Joint3DComponent::JointType::Hinge;
        joint.Axis = {0.0f, 0.0f, 1.0f};
        joint.LimitsEnabled = true;
        joint.LimitMin = -0.5f;
        joint.LimitMax = 0.5f;
        joint.Motor = Joint3DComponent::MotorMode::Velocity;
        joint.MotorTarget = 5.0f;
        joint.MotorMaxForce = 20.0f;
        scene->OnRuntimeStart();
        float highest = 0.0f;
        for (int i = 0; i < static_cast<int>(kRate); ++i)
        {
            scene->StepSimulation();
            highest = std::max(highest, arm.GetComponent<Joint3DComponent>().RuntimePosition);
        }
        CHECK(highest < 0.52f && highest > 0.45f, "a hinge limit stops a motor driving past it");
        // Re-targeted at run time, as a script would: position mode back to -0.3 rad.
        joint.Motor = Joint3DComponent::MotorMode::Position;
        joint.MotorTarget = -0.3f;
        Physics::ApplyMotor(*scene, joint);
        for (int i = 0; i < static_cast<int>(kRate * 3.0f); ++i)
            scene->StepSimulation();
        const float angle = arm.GetComponent<Joint3DComponent>().RuntimePosition;
        std::cout << "  arm angle " << angle << " rad\n";
        CHECK(std::abs(angle + 0.3f) < 0.01f, "a position motor reaches a new target set while running");
        scene->OnRuntimeStop();
    }

    // A slider: a carriage driven along x to 0.5 m by a position motor.
    {
        auto scene = CreateRef<Scene>();
        scene->GetPhysicsSettings().TickRate = kRate;
        Entity carriage = MakeBody(scene, "Carriage", {0.0f, 1.0f, 0.0f}, 0.1f);
        auto& rail = carriage.AddComponent<Joint3DComponent>();
        rail.Type = Joint3DComponent::JointType::Slider;
        rail.Axis = {1.0f, 0.0f, 0.0f};
        rail.Motor = Joint3DComponent::MotorMode::Position;
        rail.MotorTarget = 0.5f;
        rail.MotorMaxForce = 200.0f;
        scene->OnRuntimeStart();
        for (int i = 0; i < static_cast<int>(kRate * 3.0f); ++i)
            scene->StepSimulation();
        const glm::vec3 at = carriage.GetComponent<TransformComponent>().Translation;
        std::cout << "  carriage at x " << at.x << ", y " << at.y << "\n";
        CHECK(std::abs(at.x - 0.5f) < 0.01f && std::abs(at.y - 1.0f) < 0.01f,
              "a slider moves only along its axis, to the motor's target, against gravity");
        scene->OnRuntimeStop();
    }

    // Two links hinged together while overlapping do not push each other apart,
    // and destroying one mid-run removes the joint cleanly.
    {
        auto scene = CreateRef<Scene>();
        scene->GetPhysicsSettings().TickRate = kRate;
        scene->GetPhysicsSettings().Gravity = {0.0f, 0.0f, 0.0f};
        Entity base = MakeBody(scene, "Base", {0.0f, 0.0f, 0.0f}, 0.5f, false);
        Entity link = MakeBody(scene, "Link", {0.3f, 0.0f, 0.0f}, 0.5f);
        auto& elbow = link.AddComponent<Joint3DComponent>();
        elbow.Type = Joint3DComponent::JointType::Ball;
        elbow.ConnectedEntity = base.GetUUID();
        elbow.Anchor = {-0.3f, 0.0f, 0.0f};
        scene->OnRuntimeStart();
        for (int i = 0; i < static_cast<int>(kRate); ++i)
            scene->StepSimulation();
        const glm::vec3 at = link.GetComponent<TransformComponent>().Translation;
        CHECK(glm::length(at - glm::vec3(0.3f, 0.0f, 0.0f)) < 0.01f,
              "jointed bodies do not collide with each other (overlapping links stay put)");
        scene->DestroyEntity(base);
        for (int i = 0; i < 10; ++i)
            scene->StepSimulation();
        CHECK(link.GetComponent<Joint3DComponent>().RuntimeConstraint == nullptr,
              "destroying a jointed body removes its joint");
        scene->OnRuntimeStop();
    }

    // Sensors: an IMU at rest reads gravity, a falling one reads free fall; a
    // ray finds the floor and names it.
    {
        auto scene = CreateRef<Scene>();
        scene->GetPhysicsSettings().TickRate = kRate;
        Entity floor = scene->CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().Translation = {0.0f, -0.5f, 0.0f};
        floor.AddComponent<RigidBody3DComponent>(); // static
        floor.AddComponent<BoxCollider3DComponent>().HalfExtent = {10.0f, 0.5f, 10.0f};
        Entity drone = MakeBody(scene, "Drone", {0.0f, 5.0f, 0.0f}, 0.2f);
        scene->OnRuntimeStart();
        for (int i = 0; i < 24; ++i)
            scene->StepSimulation();
        const glm::vec3 falling = drone.GetComponent<RigidBody3DComponent>().RuntimeLinearAcceleration;
        CHECK(std::abs(falling.y + 9.81f) < 0.05f, "a falling body's IMU reads -9.81 m/s^2");
        const auto hit =
            Physics::CastRay(*scene, drone.GetComponent<TransformComponent>().Translation, {0.0f, -1.0f, 0.0f}, 50.0f,
                             static_cast<u32>(static_cast<entt::entity>(drone)));
        const float height = drone.GetComponent<TransformComponent>().Translation.y;
        CHECK(hit.has_value() && std::abs(hit->Distance - height) < 0.01f &&
                  hit->Entity == static_cast<u32>(static_cast<entt::entity>(floor)) && hit->Normal.y > 0.99f,
              "a downward ray finds the floor at the drone's height and names it");
        CHECK(!Physics::CastRay(*scene, {3.0f, 5.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 50.0f).has_value(),
              "a ray into the empty sky hits nothing");
        for (int i = 0; i < static_cast<int>(kRate * 3.0f); ++i)
            scene->StepSimulation();
        const glm::vec3 resting = drone.GetComponent<RigidBody3DComponent>().RuntimeLinearAcceleration;
        CHECK(glm::length(resting) < 0.05f, "at rest on the floor its acceleration is zero");
        scene->OnRuntimeStop();
    }
}

/// The sequencer's timeline logic: sampling, keys, playback and events.
static void TestSequence()
{
    std::cout << "TestSequence\n";
    SequenceTrack track;
    CHECK(Sequence::Sample(track, 1.0f, glm::vec3(7.0f)) == glm::vec3(7.0f), "no keys: the fallback");
    Sequence::SetKey(track, 2.0f, glm::vec3(10.0f), SequenceInterp::Linear);
    Sequence::SetKey(track, 0.0f, glm::vec3(0.0f), SequenceInterp::Linear);
    CHECK(track.Keys.size() == 2 && track.Keys[0].Time == 0.0f, "keys stay sorted on insert");
    CHECK(Sequence::SetKey(track, 2.0002f, glm::vec3(20.0f)) == 1 && track.Keys.size() == 2 &&
              track.Keys[1].Value == glm::vec3(20.0f),
          "a key at the same time is replaced, not duplicated");
    CHECK(std::abs(Sequence::Sample(track, 1.0f, {}).x - 10.0f) < 1e-5f, "linear halfway");
    CHECK(Sequence::Sample(track, -1.0f, {}).x == 0.0f && Sequence::Sample(track, 9.0f, {}).x == 20.0f,
          "held before the first key and after the last");
    track.Keys[0].Interp = SequenceInterp::Step;
    CHECK(Sequence::Sample(track, 1.9f, {}).x == 0.0f, "step holds until the next key");

    // Smooth passes through every key and has no corner at a middle key.
    SequenceTrack smooth;
    Sequence::SetKey(smooth, 0.0f, glm::vec3(0.0f));
    Sequence::SetKey(smooth, 1.0f, glm::vec3(1.0f));
    Sequence::SetKey(smooth, 2.0f, glm::vec3(0.0f));
    CHECK(std::abs(Sequence::Sample(smooth, 1.0f, {}).x - 1.0f) < 1e-5f, "smooth passes through its keys");
    const float before = Sequence::Sample(smooth, 0.999f, {}).x;
    const float after = Sequence::Sample(smooth, 1.001f, {}).x;
    CHECK(std::abs(before - after) < 1e-3f, "smooth: no corner at the middle key");
    CHECK(Sequence::Sample(smooth, 0.5f, {}).x > 0.5f, "smooth bulges towards the peak, unlike linear");

    // Playback: once through, then stopped at the end.
    Sequence sequence;
    sequence.Duration = 2.0f;
    SequenceTrack events;
    events.Property = SequenceProperty::Event;
    for (const auto& [t, name] : {std::pair{0.0f, "start"}, {0.5f, "half"}, {1.5f, "late"}, {2.0f, "end"}})
    {
        SequenceKey key;
        key.Time = t;
        key.Event = name;
        events.Keys.push_back(key);
    }
    sequence.Tracks.push_back(events);
    float time = 0.0f;
    bool playing = true;
    std::vector<std::string> fired;
    sequence.Advance(time, playing, 0.6f, &fired);
    CHECK(fired == std::vector<std::string>({"start", "half"}), "a key at 0 fires when playback starts there");
    fired.clear();
    sequence.Advance(time, playing, 5.0f, &fired);
    CHECK(fired == std::vector<std::string>({"late", "end"}) && !playing && time == 2.0f,
          "once through: stops at the end after the last events");

    // Looping: a step across the end fires the events on both sides, in order.
    sequence.Loop = true;
    time = 1.4f;
    playing = true;
    fired.clear();
    sequence.Advance(time, playing, 1.2f, &fired);
    CHECK(fired == std::vector<std::string>({"late", "end", "start", "half"}) && playing &&
              std::abs(time - 0.6f) < 1e-5f,
          "a loop wraps and fires across the wrap in order");
    fired.clear();
    sequence.Advance(time, playing, -1.0f, &fired);
    CHECK(fired.empty() && std::abs(time - 1.6f) < 1e-5f, "reverse playback wraps back without events");

    // In a scene: tracks move their targets by UUID; a missing target is skipped.
    auto scene = CreateRef<Scene>();
    Entity door = scene->CreateEntity("Door");
    SequenceTrack slide;
    slide.Target = door.GetUUID();
    Sequence::SetKey(slide, 0.0f, glm::vec3(0.0f), SequenceInterp::Linear);
    Sequence::SetKey(slide, 1.0f, glm::vec3(0.0f, 3.0f, 0.0f), SequenceInterp::Linear);
    SequenceTrack ghost = slide;
    ghost.Target = 12345;
    Sequence opening;
    opening.Duration = 1.0f;
    opening.Tracks = {slide, ghost};
    scene->ApplySequence(opening, 0.5f);
    CHECK(std::abs(door.GetComponent<TransformComponent>().Translation.y - 1.5f) < 1e-5f,
          "ApplySequence moves the target");
}

static void TestUI()
{
    std::cout << "[ui]\n";
    auto scene = CreateRef<Scene>();
    const glm::vec2 screen(800.0f, 600.0f);
    const auto closeTo = [](glm::vec2 a, glm::vec2 b) { return glm::length(a - b) < 1e-3f; };

    // Pinned to the top-right corner, 20 px in from both edges.
    Entity corner = scene->CreateEntity("Corner");
    auto& cr = corner.AddComponent<UIRectComponent>();
    cr.AnchorMin = cr.AnchorMax = {1.0f, 0.0f};
    cr.Pivot = {1.0f, 0.0f};
    cr.Position = {-20.0f, 20.0f};
    cr.Size = {100.0f, 30.0f};
    // Stretched across the bottom, 10 px margins, 50 px tall.
    Entity bar = scene->CreateEntity("Bottom");
    auto& br = bar.AddComponent<UIRectComponent>();
    br.AnchorMin = {0.0f, 1.0f};
    br.AnchorMax = {1.0f, 1.0f};
    br.Pivot = {0.5f, 1.0f};
    br.Position = {0.0f, -10.0f};
    br.Size = {-20.0f, 50.0f};
    bar.AddComponent<UIImageComponent>();
    // A centred panel with a button in it.
    Entity panel = scene->CreateEntity("Panel");
    auto& pr = panel.AddComponent<UIRectComponent>();
    pr.Size = {300.0f, 200.0f};
    panel.AddComponent<UIImageComponent>();
    Entity button = scene->CreateChildEntity(panel, "Button");
    auto& bt = button.AddComponent<UIRectComponent>();
    bt.AnchorMin = bt.AnchorMax = {0.5f, 1.0f};
    bt.Pivot = {0.5f, 1.0f};
    bt.Position = {0.0f, -10.0f};
    bt.Size = {120.0f, 40.0f};
    button.AddComponent<UIImageComponent>();
    auto& bc = button.AddComponent<UIButtonComponent>();
    button.AddComponent<UITextComponent>().Text = "Go";
    scene->OnUpdateEditorSimulation(Timestep(0.016f)); // applies the deferred child attach

    UI::Pointer idle;
    std::vector<UI::DrawCommand> list = UI::Update(*scene, screen, idle);
    CHECK(closeTo(cr.ComputedMin, {680.0f, 20.0f}) && closeTo(cr.ComputedMax, {780.0f, 50.0f}),
          "a corner anchor keeps its distance from that corner");
    CHECK(closeTo(br.ComputedMin, {10.0f, 540.0f}) && closeTo(br.ComputedMax, {790.0f, 590.0f}),
          "stretched anchors follow the screen's width, minus the margins");
    CHECK(closeTo(pr.ComputedMin, {250.0f, 200.0f}), "a centre anchor centres");
    CHECK(closeTo(bt.ComputedMin, {340.0f, 350.0f}) && closeTo(bt.ComputedMax, {460.0f, 390.0f}),
          "a child is laid out inside its parent");
    UI::Update(*scene, {1000.0f, 600.0f}, idle);
    CHECK(closeTo(cr.ComputedMin, {880.0f, 20.0f}), "and moves with the corner when the screen resizes");
    list = UI::Update(*scene, screen, idle);
    CHECK(list.size() == 4 && list.back().Type == UI::DrawCommand::Kind::Text && list.back().Text == "Go",
          "the draw list is back to front: bar, panel, button, its text");

    // Clicks: press and release on the button, with the pointer active.
    UI::Pointer over;
    over.Active = true;
    over.Position = {400.0f, 370.0f};
    UI::Events events;
    UI::Update(*scene, screen, over, &events);
    CHECK(bc.Hovered && !bc.Pressed && events.PointerOverUI, "hovering marks the button");
    over.Down = over.Pressed = true;
    UI::Update(*scene, screen, over, &events);
    over.Pressed = false;
    CHECK(bc.Pressed, "pressing holds it down");
    over.Down = false;
    over.Released = true;
    events = {};
    UI::Update(*scene, screen, over, &events);
    over.Released = false;
    CHECK(bc.Clicks == 1 && events.Clicked.size() == 1 && events.Clicked[0] == static_cast<entt::entity>(button),
          "a press and release on it is one click");
    // Pressed on the button, released elsewhere: no click.
    over.Down = over.Pressed = true;
    UI::Update(*scene, screen, over);
    over.Pressed = false;
    over.Position = {50.0f, 50.0f};
    over.Down = false;
    over.Released = true;
    UI::Update(*scene, screen, over);
    over.Released = false;
    CHECK(bc.Clicks == 1, "releasing off the button cancels the click");
    // Hidden: neither drawn nor clickable.
    pr.Visible = false;
    over.Position = {400.0f, 370.0f};
    list = UI::Update(*scene, screen, over);
    CHECK(!bc.Hovered && list.size() == 1, "a hidden panel hides its children too");
    pr.Visible = true;

    // A slider follows the mouse from the press, even past its ends.
    Entity slider = scene->CreateEntity("Slider");
    auto& sr = slider.AddComponent<UIRectComponent>();
    sr.AnchorMin = sr.AnchorMax = {0.0f, 0.0f};
    sr.Pivot = {0.0f, 0.0f};
    sr.Position = {100.0f, 100.0f};
    sr.Size = {200.0f, 20.0f};
    sr.Order = 10;
    auto& sb = slider.AddComponent<UIProgressBarComponent>();
    sb.Interactable = true;
    UI::Pointer drag;
    drag.Active = true;
    drag.Position = {150.0f, 110.0f};
    drag.Down = drag.Pressed = true;
    UI::Update(*scene, screen, drag);
    CHECK(std::fabs(sb.Value - 0.25f) < 1e-4f && sb.Dragging, "pressing a slider sets its value there");
    drag.Pressed = false;
    drag.Position = {500.0f, 300.0f};
    UI::Update(*scene, screen, drag);
    CHECK(sb.Value == 1.0f, "dragging past the end clamps to 1");
    drag.Down = false;
    drag.Released = true;
    UI::Update(*scene, screen, drag);
    CHECK(!sb.Dragging, "releasing ends the drag");
    list = UI::Update(*scene, screen, idle);
    CHECK(list.size() == 6 && list[4].Max.x == 300.0f && list[5].Max.x == 300.0f,
          "a bar draws its background and a fill as wide as its value");

    // Scaled UI: laid out at 720 px tall, drawn at 1440: everything doubles,
    // and the mouse is mapped back (the button is under the same spot).
    CHECK(UI::Scale(true, 720.0f, {2560.0f, 1440.0f}) == 2.0f && UI::Scale(false, 720.0f, {2560.0f, 1440.0f}) == 1.0f,
          "scale is the screen height over the reference height, when on");
    CHECK(UI::Scale(true, 720.0f, {3440.0f, 1440.0f}) == 2.0f, "a wider screen is fitted by its height");
    CHECK(UI::Scale(true, 720.0f, {640.0f, 1440.0f}) == 0.5f, "a portrait screen is fitted by its width");
    list = UI::Update(*scene, {1600.0f, 1200.0f}, idle, nullptr, 2.0f);
    CHECK(closeTo(cr.ComputedMin, {680.0f, 20.0f}), "rects stay in UI pixels");
    const auto cornerBox = std::find_if(list.begin(), list.end(), [&](const UI::DrawCommand& c)
                                        { return c.Entity == static_cast<entt::entity>(button); });
    CHECK(cornerBox != list.end() && closeTo(cornerBox->Min, {680.0f, 700.0f}) && cornerBox->CornerRadius == 12.0f,
          "the draw list is scaled to the screen");
    UI::Pointer scaled;
    scaled.Active = true;
    scaled.Position = {800.0f, 740.0f}; // the button's centre at twice the size
    UI::Update(*scene, {1600.0f, 1200.0f}, scaled, nullptr, 2.0f);
    CHECK(bc.Hovered, "the pointer is mapped into UI pixels");
}

int main()
{
    Scene::GetEditorOverlays().LightIcons = false; // no texture loading in headless tests
    Physics::PhysicsSystem3D::Init();   // Jolt global registration (Factory/RegisterTypes)

    TestCreationAndIndex();
    TestSceneCameraClipPlanes();
    TestTerrain();
    TestStaleEntityHandles();
    TestUI();
    TestSequence();
    TestRobotJoints();
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
