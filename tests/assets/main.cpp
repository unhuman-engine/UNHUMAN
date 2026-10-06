// No-GPU asset system harness (issue #38).
//
// Covers UUID uniqueness, the AssetManager registry (path->ID dedup,
// refcounted handles, load state machine, sync + async loading through the
// real Jobsystem, hot reload) and the mount-based VFS. No GPU involved: the
// manager ships Text/Binary loaders, which are pure file IO.
#include "UHE/AssestsManager/AssetManager.h"
#include "UHE/AssestsManager/VfsSystem.h"
#include "UHE/Core/UUID.h"
#include "UHE/Jobsystem/Jobsystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_set>
#include <unistd.h>

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

namespace fs = std::filesystem;

static fs::path MakeTempDir()
{
    static int counter = 0;
    fs::path dir = fs::temp_directory_path() /
                   ("uhe_asset_test_" + std::to_string(counter++) + "_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    return dir;
}

static void WriteFile(const fs::path& path, const std::string& content)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

static void TestUUIDUniqueness()
{
    std::cout << "[uuid]\n";
    constexpr int count = 10000;
    std::unordered_set<u64> seen;
    seen.reserve(count);
    bool unique = true;
    for (int i = 0; i < count; ++i)
        if (!seen.insert(UUID()).second)
            unique = false;
    CHECK(unique, "10000 minted UUIDs are unique");
    CHECK(UUID(42) == UUID(42), "explicit-value UUIDs compare equal");
    CHECK(UUID(7) != UUID(8), "different values compare unequal");
}

static void TestSyncLoad()
{
    std::cout << "[sync load]\n";
    fs::path dir = MakeTempDir();
    fs::path file = dir / "greeting.txt";
    WriteFile(file, "hello unhuman");

    auto& manager = AssetManager::Get();
    AssetHandle<TextAsset> handle = manager.Load<TextAsset>(file.string());
    CHECK(manager.FindByPath(file.string()) == handle.ID(), "path maps to the asset ID");
    CHECK(handle.IsValid(), "sync load produces a valid handle");
    CHECK(handle.Get() && handle.Get()->Content == "hello unhuman", "text content decoded");

    // Path -> ID dedup: a second request shares the record.
    AssetHandle<TextAsset> again = manager.Load<TextAsset>(file.string());
    CHECK(again.ID() == handle.ID(), "second load dedups to the same ID");

    // Handles are refcounted.
    CHECK(manager.RegisteredCount() == 1, "one record registered");
    AssetHandle<TextAsset> copy = handle;
    handle = AssetHandle<TextAsset>();
    again = AssetHandle<TextAsset>();
    manager.UnloadUnused();
    CHECK(manager.RegisteredCount() == 1, "record survives while a handle lives");
    CHECK(copy.IsValid() && copy.Get()->Content == "hello unhuman", "surviving handle still valid");
    copy = AssetHandle<TextAsset>();
    manager.UnloadUnused();
    CHECK(manager.RegisteredCount() == 0, "record reclaimed once handles are gone");

    fs::remove_all(dir);
}

static void TestFailedLoad()
{
    std::cout << "[failed load]\n";
    auto& manager = AssetManager::Get();
    AssetID id;
    {
        AssetHandle<TextAsset> handle = manager.Load<TextAsset>("no/such/file.txt");
        CHECK(!handle.IsValid(), "missing file yields an invalid handle");
        id = handle.ID();
        CHECK(manager.GetState(id) == LoadState::Failed, "state is Failed");
    }
    manager.UnloadUnused();
    CHECK(manager.RegisteredCount() == 0, "failed record reclaimed");
}

static void TestAsyncLoad()
{
    std::cout << "[async load]\n";
    fs::path dir = MakeTempDir();
    fs::path file = dir / "big.bin";
    std::string payload(4096, 'x');
    WriteFile(file, payload);

    Jobsystem::UheJobsystem jobs;
    jobs.Init();
    auto& manager = AssetManager::Get();
    manager.SetJobsystem(&jobs);

    bool callbackFired = false;
    bool callbackSuccess = false;
    AssetHandle<BinaryAsset> handle =
        manager.LoadAsync<BinaryAsset>(file.string(), [&](AssetID, bool ok)
                                       { callbackFired = true; callbackSuccess = ok; });

    // Spin Update() until the callback is delivered.
    for (int i = 0; i < 1000 && !callbackFired; ++i)
    {
        manager.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(callbackFired, "callback delivered on the main thread");
    CHECK(callbackSuccess, "callback reports success");
    CHECK(handle.IsValid(), "handle valid after async completion");
    CHECK(handle.Get() && handle.Get()->Bytes.size() == payload.size(), "binary content decoded");

    // Re-request while ready shares the record instead of re-decoding.
    AssetHandle<BinaryAsset> again = manager.LoadAsync<BinaryAsset>(file.string());
    CHECK(again.ID() == handle.ID(), "async re-request dedups");

    again = AssetHandle<BinaryAsset>();
    handle = AssetHandle<BinaryAsset>();
    manager.SetJobsystem(nullptr);
    jobs.ShutDown();
    manager.UnloadUnused();
    CHECK(manager.RegisteredCount() == 0, "async record reclaimed");

    fs::remove_all(dir);
}

static void TestUnloadWithLiveHandle()
{
    std::cout << "[unload]\n";
    fs::path dir = MakeTempDir();
    WriteFile(dir / "a.txt", "aaa");

    auto& manager = AssetManager::Get();
    AssetHandle<TextAsset> handle = manager.Load<TextAsset>((dir / "a.txt").string());

    manager.Unload(handle.ID());
    manager.Update();
    CHECK(manager.RegisteredCount() == 1, "unload waits for the last handle");
    handle = AssetHandle<TextAsset>();
    manager.Update();
    CHECK(manager.RegisteredCount() == 0, "record reclaimed after the handle drains");

    fs::remove_all(dir);
}

static void TestHotReload()
{
    std::cout << "[hot reload]\n";
    fs::path dir = MakeTempDir();
    fs::path file = dir / "cfg.txt";
    WriteFile(file, "version 1");

    auto& manager = AssetManager::Get();
    AssetHandle<TextAsset> handle = manager.Load<TextAsset>(file.string());
    CHECK(handle.IsValid(), "initial load valid");

    int notified = 0;
    manager.AddListener(handle.ID(), [&] { ++notified; });
    manager.SetHotReloadEnabled(true);

    // Change the content and push the mtime forward so the poll sees it.
    WriteFile(file, "version 2");
    auto futureTime = fs::last_write_time(file) + std::chrono::seconds(10);
    fs::last_write_time(file, futureTime);

    manager.Update();
    CHECK(notified == 1, "listener notified after reload");

    // The old generation is stale; a fresh load sees the new content.
    CHECK(!handle.IsValid(), "stale handle invalidated by the generation bump");
    AssetHandle<TextAsset> fresh = manager.Load<TextAsset>(file.string());
    CHECK(fresh.IsValid() && fresh.Get()->Content == "version 2", "reload delivered new content");
    CHECK(fresh.ID() == handle.ID(), "reload keeps the same asset ID");

    fresh = AssetHandle<TextAsset>();
    handle = AssetHandle<TextAsset>();
    manager.SetHotReloadEnabled(false);
    manager.UnloadUnused();
    fs::remove_all(dir);
}

static void TestVfs()
{
    std::cout << "[vfs]\n";
    Vfs& vfs = Vfs::Get();

    fs::path base = MakeTempDir();
    fs::path mod = MakeTempDir();
    fs::create_directories(base / "models");

    vfs.Mount("base:", base);
    vfs.Mount("mod:", mod);

    auto resolved = vfs.Resolve("base:/models/gun.glb");
    CHECK(resolved.has_value(), "mount resolves a virtual path");
    CHECK(*resolved == fs::absolute(base / "models" / "gun.glb").lexically_normal(),
          "path maps under the mount root");

    // Remount replaces the physical root without duplicating the entry.
    vfs.Mount("layer:", base);
    vfs.Mount("layer:", mod);
    auto layers = vfs.ResolveAll("layer:/models/gun.glb");
    CHECK(layers.size() == 1, "remount replaces instead of duplicating");
    CHECK(layers[0] == fs::absolute(mod / "models" / "gun.glb").lexically_normal(),
          "remount points at the new root");

    // Prefix safety: "game:" must not swallow "gameboy:/x".
    vfs.Mount("game:", base);
    CHECK(!vfs.Resolve("gameboy:/x").has_value(), "longer prefixes do not match shorter mounts");
    CHECK(vfs.Resolve("game:x").has_value(), "paths without a separator still resolve");

    CHECK(!vfs.Resolve("nomount:/x").has_value(), "unknown mounts do not resolve");

    fs::remove_all(base);
    fs::remove_all(mod);
}

static void TestFileSystemShim(const char* argv0)
{
    std::cout << "[filesystem shim]\n";
    // Initialize discovers the project root from the executable location and
    // mounts it as "game:".
    FileSystem::Get().Initialize(argv0);
    bool hasGameMount = false;
    for (const auto& [root, physical] : Vfs::Get().GetMounts())
        if (root == "game:")
            hasGameMount = true;
    CHECK(hasGameMount, "Initialize mounted the game root");
    CHECK(!FileSystem::Get().GetRootPath().empty(), "legacy root path still available");

    // Legacy prefixless Resolve forwards through the "game:" mount.
    std::string resolved = FileSystem::Get().Resolve("icon/pointLight.png");
    CHECK(resolved.find("assets/icon/pointLight.png") != std::string::npos,
          "legacy path resolves through the mount");
}

int main(int argc, char** argv)
{
    TestUUIDUniqueness();
    TestSyncLoad();
    TestFailedLoad();
    TestAsyncLoad();
    TestUnloadWithLiveHandle();
    TestHotReload();
    TestVfs();
    TestFileSystemShim(argc > 0 ? argv[0] : ".");

    if (g_failures == 0)
    {
        std::cout << "\nALL ASSET TESTS PASSED\n";
        return 0;
    }
    std::cout << "\n" << g_failures << " ASSET TEST(S) FAILED\n";
    return 1;
}
