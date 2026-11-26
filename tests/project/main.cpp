// Project system harness: creates, loads, saves and mounts a project, and checks
// that asset paths resolve through the project ("game:") and engine ("engine:")
// mounts. No GPU, no ImGui.
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "UHE/AssetsManager/AssetImport.h"
#include "UHE/AssetsManager/GameExporter.h"
#include "UHE/AssetsManager/Project.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Core/CommandLine.h"

namespace fs = std::filesystem;
using namespace UHE;

static int g_failures = 0;

static void Check(bool ok, const char* msg)
{
    std::printf(ok ? "ok   : %s\n" : "FAIL : %s\n", msg);
    if (!ok)
        ++g_failures;
}

static void WriteFile(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
}

int main()
{
    const fs::path sandbox = fs::temp_directory_path() / "uhe_project_test";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);

    // ── create ──────────────────────────────────────────────────────────────
    const fs::path gameRoot = sandbox / "GameSnake";
    ProjectInfo snake;
    std::string error;
    Check(Project::Create(gameRoot, "GameSnake", snake, &error), "create: a new project is written");
    Check(error.empty(), "create: no error reported");
    Check(fs::exists(gameRoot / Project::kManifestName), "create: manifest exists");
    Check(fs::is_directory(gameRoot / Project::kAssetFolderName), "create: Assets folder exists");
    Check(snake.Name == "GameSnake", "create: name is stored");
    Check(snake.AssetPath == gameRoot / Project::kAssetFolderName, "create: asset path points at Assets/");

    Check(Project::IsProject(gameRoot), "is-project: recognises a project folder");
    Check(Project::IsProject(gameRoot / Project::kManifestName), "is-project: accepts the manifest path too");
    Check(!Project::IsProject(sandbox), "is-project: rejects a plain folder");

    error.clear();
    Check(!Project::Create(gameRoot, "GameSnake", snake, &error), "create: refuses to overwrite");
    Check(!error.empty(), "create: reports why it refused");

    // ── load / save ─────────────────────────────────────────────────────────
    ProjectInfo loaded;
    Check(Project::Load(gameRoot, loaded, &error), "load: reads the manifest back");
    Check(loaded.Name == "GameSnake", "load: name round-trips");
    Check(loaded.RootPath == gameRoot, "load: root is the manifest's folder");
    Check(loaded.StartScene.empty(), "load: start scene defaults to empty");

    loaded.StartScene = "Assets/Scenes/Main.unhuman";
    Check(Project::Save(loaded, &error), "save: writes the manifest");
    ProjectInfo reloaded;
    Check(Project::Load(gameRoot, reloaded, &error) && reloaded.StartScene == loaded.StartScene,
          "save: start scene round-trips");
    Check(Project::StartScenePath(reloaded) == gameRoot / "Assets/Scenes/Main.unhuman",
          "start scene: resolved against the project root");

    // A manifest without a name still opens, using the folder name.
    const fs::path unnamed = sandbox / "Unnamed";
    WriteFile(unnamed / Project::kManifestName, "Project:\n  EngineVersion: \"0.1\"\n");
    ProjectInfo fallback;
    Check(Project::Load(unnamed, fallback, &error) && fallback.Name == "Unnamed",
          "load: a nameless manifest falls back to the folder name");
    Check(fallback.Window.Width == 1280 && fallback.Window.Height == 720 &&
              fallback.Window.Mode == WindowMode::Windowed && fallback.Window.VSync && fallback.Window.Title.empty(),
          "window: a manifest without a Window block gets the defaults");

    // ── one spelling per project folder ─────────────────────────────────────
    {
        const fs::path root = Project::NormalizeRoot(gameRoot);
        Check(root.is_absolute(), "normalize: absolute");
        Check(Project::NormalizeRoot(gameRoot.string() + "/") == root, "normalize: a trailing slash is dropped");
        Check(Project::NormalizeRoot(gameRoot / Project::kManifestName) == root, "normalize: the manifest path too");
        Check(Project::NormalizeRoot(gameRoot / "Assets" / "..") == root, "normalize: dot-dot is resolved");
        const fs::path previous = fs::current_path();
        fs::current_path(sandbox);
        ProjectInfo relative;
        Check(Project::Load("GameSnake", relative, &error) && relative.RootPath == root &&
                  relative.AssetPath == root / Project::kAssetFolderName,
              "load: a relative path gives an absolute root");
        Check(Project::NormalizeRoot("GameSnake") == root, "normalize: relative to the working directory");
        fs::current_path(previous);
    }

    // ── checking a new project's name and folder ────────────────────────────
    {
        const fs::path parent = sandbox / "new_projects";
        fs::create_directories(parent);
        Check(Project::CheckNewProject(parent, "My Game").empty(), "new: a plain name in an empty spot is fine");
        Check(!Project::CheckNewProject(parent, "").empty(), "new: an empty name is refused");
        Check(!Project::CheckNewProject(parent, "a/b").empty() && !Project::CheckNewProject(parent, "a\\b").empty(),
              "new: separators are refused");
        Check(!Project::CheckNewProject(parent, "What?").empty() && !Project::CheckNewProject(parent, "x:y").empty(),
              "new: characters Windows forbids are refused");
        Check(!Project::CheckNewProject(parent, "..").empty() && !Project::CheckNewProject(parent, " pad").empty() &&
                  !Project::CheckNewProject(parent, "dot.").empty(),
              "new: '..', leading spaces and a trailing dot are refused");
        Check(!Project::CheckNewProject({}, "Game").empty(), "new: no folder is refused");
        WriteFile(parent / "Busy" / "notes.txt", "mine");
        Check(!Project::CheckNewProject(parent, "Busy").empty(), "new: a folder with files in it is refused");
        fs::create_directories(parent / "Empty");
        Check(Project::CheckNewProject(parent, "Empty").empty(), "new: an existing empty folder is fine");
        Check(!Project::CheckNewProject(sandbox, "GameSnake").empty(), "new: an existing project is refused");
    }

    // ── the game window ─────────────────────────────────────────────────────
    reloaded.Window = {.Title = "Snake!", .Width = 1600, .Height = 900, .Mode = WindowMode::Borderless, .VSync = false};
    Check(Project::Save(reloaded, &error), "window: saved with the manifest");
    ProjectInfo windowed;
    Check(Project::Load(gameRoot, windowed, &error) && windowed.Window.Title == "Snake!" &&
              windowed.Window.Width == 1600 && windowed.Window.Height == 900 &&
              windowed.Window.Mode == WindowMode::Borderless && !windowed.Window.VSync,
          "window: every field round-trips");

    const fs::path oddWindow = sandbox / "windows" / "OddWindow"; // not in the scanned folder
    WriteFile(oddWindow / Project::kManifestName, "Project:\n  Name: Odd\n  Window:\n    Width: 0\n"
                                                  "    Height: 999999\n    Mode: FULLSCREEN\n    VSync: maybe\n");
    ProjectInfo odd;
    Check(Project::Load(oddWindow, odd, &error), "window: an odd block still loads");
    Check(odd.Window.Width == ProjectWindowSettings::kMinSize && odd.Window.Height == ProjectWindowSettings::kMaxSize,
          "window: sizes are clamped");
    Check(odd.Window.Mode == WindowMode::Fullscreen, "window: the mode is read in any case");
    Check(odd.Window.VSync, "window: an unreadable value keeps its default");
    Check(ParseWindowMode("Exclusive", WindowMode::Windowed) == WindowMode::Windowed,
          "window: an unknown mode falls back");

    // ── failure modes ───────────────────────────────────────────────────────
    error.clear();
    Check(!Project::Load(sandbox, fallback, &error), "load: fails without a manifest");
    Check(!error.empty(), "load: explains the missing manifest");

    const fs::path broken = sandbox / "Broken";
    WriteFile(broken / Project::kManifestName, "Project: [this is not a map\n");
    error.clear();
    Check(!Project::Load(broken, fallback, &error), "load: fails on a corrupt manifest");
    Check(!error.empty(), "load: explains the corrupt manifest");

    // A project whose Assets folder was deleted gets it back.
    const fs::path droneRoot = sandbox / "SimulationDrone";
    ProjectInfo drone;
    Check(Project::Create(droneRoot, "SimulationDrone", drone, &error), "create: second project");
    fs::remove_all(droneRoot / Project::kAssetFolderName, ec);
    Check(Project::EnsureAssetFolder(drone, &error) && fs::is_directory(drone.AssetPath),
          "ensure: recreates a missing Assets folder");

    // ── mounting + asset resolution ─────────────────────────────────────────
    WriteFile(snake.AssetPath / "Textures" / "wall.png", "project texture");
    WriteFile(snake.AssetPath / "shared.txt", "project copy");
    const fs::path engineAssets = sandbox / "engine_assets";
    WriteFile(engineAssets / "shared.txt", "engine copy");
    WriteFile(engineAssets / "shaders" / "basic.slang", "engine shader");

    Project::Mount(snake, engineAssets);

    const fs::path projectHit = ResolveAssetPath("Textures/wall.png");
    Check(projectHit == snake.AssetPath / "Textures" / "wall.png",
          "resolve: a project-relative path lands inside the project");
    Check(ResolveAssetPath("shaders/basic.slang") == engineAssets / "shaders" / "basic.slang",
          "resolve: engine assets stay reachable through the engine mount");
    Check(ResolveAssetPath("shared.txt") == snake.AssetPath / "shared.txt",
          "resolve: the project wins over the engine folder for the same name");

    const std::string absolute = (engineAssets / "shaders" / "basic.slang").string();
    Check(ResolveAssetPath(absolute) == fs::path(absolute), "resolve: absolute paths pass through");
    Check(ResolveAssetPath("") == fs::path(), "resolve: an empty path stays empty");
    Check(ResolveAssetPath("missing.png") == fs::path("missing.png"),
          "resolve: an unresolved path is returned unchanged");

    const fs::path escaped = ResolveAssetPath("../escape.png");
    Check(!fs::exists(escaped), "resolve: traversal cannot escape the mounts");

    // ── command line ────────────────────────────────────────────────────────
    {
        std::array<const char*, 5> argv{"UHE_EDITOR", "-project", "/tmp/SomeProject", "--launcher", "-verbose=1"};
        CommandLine::Init(static_cast<int>(argv.size()), const_cast<char**>(argv.data()));
        Check(CommandLine::GetValue("-project") == "/tmp/SomeProject", "cli: -project <path>");
        Check(CommandLine::GetValue("--project") == "/tmp/SomeProject", "cli: option names ignore dashes");
        Check(CommandLine::GetValue("-nope").empty(), "cli: absent option has no value");
        Check(CommandLine::HasFlag("-launcher") && CommandLine::HasFlag("launcher"), "cli: flags match both forms");
        Check(!CommandLine::HasFlag("-nope"), "cli: unknown flag is not present");
        Check(CommandLine::GetArgs().size() == argv.size() - 1, "cli: argv[0] is not stored");

        std::array<const char*, 2> attached{"UHE_EDITOR", "-project=/tmp/Attached"};
        CommandLine::Init(static_cast<int>(attached.size()), const_cast<char**>(attached.data()));
        Check(CommandLine::GetValue("-project") == "/tmp/Attached", "cli: -option=value form");
    }

    // ── folder scan ─────────────────────────────────────────────────────────
    // The launcher scans the same folder through two spellings; that must not
    // produce duplicate entries.
    {
        std::vector<ProjectInfo> found;
        // The sandbox holds GameSnake, SimulationDrone and Unnamed (a manifest
        // without a name); Broken has a corrupt manifest and is skipped.
        Check(Project::ScanFolder(sandbox, found) == 3, "scan: finds every project in a folder");
        Check(Project::ScanFolder(sandbox, found) == 0, "scan: a second scan of the same folder adds nothing");
        Check(Project::ScanFolder(sandbox / "GameSnake", found) == 0,
              "scan: a project is not itself a folder of projects");
        Check(Project::ScanFolder(sandbox / "does_not_exist", found) == 0, "scan: a missing folder adds nothing");
        Check(found.size() == 3, "scan: the list stays deduplicated");
        Check(std::none_of(found.begin(), found.end(), [](const ProjectInfo& info) { return info.Name == "Broken"; }),
              "scan: folders that are not projects are skipped");
    }

    // ── asset import (OS drops and the Content Browser's Import button) ─────
    {
        const fs::path incoming = sandbox / "incoming";
        WriteFile(incoming / "wall.png", "dropped texture");
        const fs::path target = sandbox / "ImportTarget";
        std::string error;

        const fs::path first = ImportAssetFile(incoming / "wall.png", target, &error);
        Check(first == target / "wall.png" && fs::exists(first), "import: copies the file into the folder");
        Check(fs::file_size(first) == fs::file_size(incoming / "wall.png"), "import: the copy is complete");

        const fs::path second = ImportAssetFile(incoming / "wall.png", target, &error);
        Check(second == target / "wall_1.png" && fs::exists(second),
              "import: a name clash is numbered instead of overwritten");

        Check(ImportAssetFile(first, target, &error) == first, "import: re-importing an owned file is a no-op");

        error.clear();
        Check(ImportAssetFile(sandbox / "missing.png", target, &error).empty(), "import: a missing source fails");
        Check(!error.empty(), "import: the failure is explained");

        Check(MakeAssetRelativePath(first, target) == "wall.png", "import: project-owned file gets a relative path");
        Check(MakeAssetRelativePath(incoming / "wall.png", target) == (incoming / "wall.png").generic_string(),
              "import: a file outside the base keeps its full path");
    }

    // ── shipped examples ────────────────────────────────────────────────────
    // The repository ships starter projects; they must stay loadable.
    {
        const fs::path examplesRoot = fs::path(UHE_REPO_DIR) / "projects";
        std::size_t exampleCount = 0;
        std::error_code iterEc;
        if (fs::is_directory(examplesRoot, iterEc))
        {
            for (const fs::directory_entry& entry : fs::directory_iterator(examplesRoot, iterEc))
            {
                if (!entry.is_directory())
                    continue;
                ProjectInfo example;
                std::string exampleError;
                const bool ok = Project::Load(entry.path(), example, &exampleError);
                if (!ok)
                    std::printf("      (example '%s' failed: %s)\n", entry.path().filename().string().c_str(),
                                exampleError.c_str());
                Check(ok && !example.Name.empty(), "examples: every projects/ folder is a loadable project");
                ++exampleCount;
            }
        }
        Check(exampleCount >= 2, "examples: the shipped starter projects are present");
    }

    // ── export a game ───────────────────────────────────────────────────────
    {
        // A stand-in build: the runtime, shared libraries, Slang's compile-only
        // modules, a linker-name stand-in and an unrelated tool.
        const fs::path bin = sandbox / "bin";
        const std::string elf = "\x7f"
                                "ELF lib";
        WriteFile(bin / "UHE_RUNTIME", "runtime");
        WriteFile(bin / "UHE_RUNTIME.exe", "runtime");
        WriteFile(bin / "libUHE.so", elf);
        WriteFile(bin / "libslang-compiler.so.0.2026.10", elf);
        WriteFile(bin / "libslang-compiler.so", "libslang-compiler.so.0.2026.10");
        WriteFile(bin / "libslang-llvm.so", elf);
        WriteFile(bin / "libslang-glslang-2026.10.so", elf);
        WriteFile(bin / "slang-glsl-module-2026.10.dll", "MZ dll");
        WriteFile(bin / "libgfx.so.0.2026.10", elf);
        WriteFile(bin / "glfw3.dll", "MZ dll");
        WriteFile(bin / "uhe_shaderc", "tool");
        const fs::path engine = sandbox / "engine_assets";
        WriteFile(engine / "shaders/Basic3D.slang", "source");
        WriteFile(engine / "shaders/Basic3D.vertex.default.spv", "spirv");
        WriteFile(engine / "shaders/variants.txt", "default:");
        WriteFile(engine / "icon/pointLight.png", "png");
        WriteFile(engine / "Inter/static/Inter_18pt-Bold.ttf", "font");
        WriteFile(engine / "Inter/static/Inter_18pt-Regular.ttf", "font");
        WriteFile(engine / "Inter/static/Inter_24pt-Thin.ttf", "unused font");
        WriteFile(engine / "Inter/OFL.txt", "font licence");
        WriteFile(engine / "model/hero.glb", "glb");
        WriteFile(engine / "model/license.txt", "model licence");
        WriteFile(engine / "model/unused.glb", "glb");

        const fs::path gameRoot2 = sandbox / "Pond Demo";
        ProjectInfo pond;
        Check(Project::Create(gameRoot2, "Pond Demo", pond, &error), "export: a project to export");
        WriteFile(pond.AssetPath / "Scenes/Pond.unhuman", "Scene: Pond\n"
                                                          "Entities:\n"
                                                          "  - Entity: 1\n"
                                                          "    TagComponent:\n"
                                                          "      Tag: Ground.gltf\n"
                                                          "    Model3DComponent:\n"
                                                          "      ModelPath: Models/Ground.gltf\n"
                                                          "  - Entity: 2\n"
                                                          "    Model3DComponent:\n"
                                                          "      ModelPath: model/hero.glb\n"
                                                          "  - Entity: 3\n"
                                                          "    SpriteRendererComponent:\n"
                                                          "      TexturePath: engine:/icon/pointLight.png\n"
                                                          "  - Entity: 4\n"
                                                          "    Model3DComponent:\n"
                                                          "      ModelPath: Models/Gone.gltf\n"
                                                          "  - Entity: 5\n"
                                                          "    Model3DComponent:\n"
                                                          "      ModelPath: /home/someone/tree.glb\n");
        WriteFile(pond.AssetPath / "Models/Ground.gltf",
                  R"({"buffers":[{"uri":"Ground%20Mesh.bin"}],"images":[{"uri":"../Textures/grass.png"},)"
                  R"({"uri":"data:image/png;base64,AAAA"},{"uri":"lost.png"}]})");
        WriteFile(pond.AssetPath / "Models/Ground Mesh.bin", "bin");
        WriteFile(pond.AssetPath / "Textures/grass.png", "png");
        WriteFile(pond.AssetPath / "Textures/unused.png", "png");
        WriteFile(pond.AssetPath / "Scripts/Spawner.lua", "local path = \"Textures/rock.png\"\nprint('hello')\n");
        WriteFile(pond.AssetPath / "Textures/rock.png", "png");
        WriteFile(pond.AssetPath / "Big/Unused.glb", "glb");

        {
            const AssetManifest manifest = GameExporter::CollectAssets(pond.AssetPath, engine);
            const auto has = [](const std::vector<fs::path>& files, const char* file)
            { return std::find(files.begin(), files.end(), fs::path(file)) != files.end(); };
            const auto warned = [&](const char* text)
            {
                return std::any_of(manifest.Warnings.begin(), manifest.Warnings.end(),
                                   [&](const std::string& w) { return w.find(text) != std::string::npos; });
            };
            Check(has(manifest.ProjectFiles, "Scenes/Pond.unhuman") &&
                      has(manifest.ProjectFiles, "Scripts/Spawner.lua"),
                  "collect: every scene and script is kept");
            Check(has(manifest.ProjectFiles, "Models/Ground.gltf"), "collect: a model a scene names is kept");
            Check(has(manifest.ProjectFiles, "Models/Ground Mesh.bin") &&
                      has(manifest.ProjectFiles, "Textures/grass.png"),
                  "collect: a glTF brings its buffers and images (decoded, relative to it)");
            Check(has(manifest.ProjectFiles, "Textures/rock.png"), "collect: a file a script names is kept");
            Check(!has(manifest.ProjectFiles, "Textures/unused.png") && !has(manifest.ProjectFiles, "Big/Unused.glb"),
                  "collect: files nothing names are left out");
            Check(has(manifest.EngineFiles, "model/hero.glb") && !has(manifest.EngineFiles, "model/unused.glb"),
                  "collect: an engine model a scene names is kept, the others are not");
            Check(has(manifest.EngineFiles, "model/license.txt") && has(manifest.EngineFiles, "Inter/OFL.txt"),
                  "collect: licences travel with their assets");
            Check(has(manifest.EngineFiles, "shaders/Basic3D.vertex.default.spv") &&
                      has(manifest.EngineFiles, "icon/pointLight.png") &&
                      has(manifest.EngineFiles, "Inter/static/Inter_18pt-Bold.ttf") &&
                      !has(manifest.EngineFiles, "Inter/static/Inter_24pt-Thin.ttf"),
                  "collect: the engine files the runtime loads itself are always in, other fonts are not");
            Check(!has(manifest.EngineFiles, "shaders/Basic3D.slang"), "collect: shader sources are left out");
            Check(warned("Models/Gone.gltf") && warned("lost.png"), "collect: a missing reference is a warning");
            Check(warned("/home/someone/tree.glb"), "collect: an absolute path is a warning");
            Check(!warned("'Ground.gltf'") && !warned("data:"), "collect: tags and data URIs are not warned about");
            Check(manifest.Warnings.size() == 3, "collect: exactly the three bad references are warned about");
        }

        ExportOptions options;
        options.RuntimeDir = bin;
        options.EngineAssets = engine;
        options.OutputDir = sandbox / "out";

        ExportReport report = GameExporter::Export(pond, options);
        Check(!report.Ok, "export: refused without a start scene");

        pond.StartScene = "Assets/Scenes/Pond.unhuman";
        report = GameExporter::Export(pond, options);
        Check(report.Ok, "export: succeeds with a start scene");
        const fs::path out = options.OutputDir;
#ifdef _WIN32
        Check(fs::exists(out / "Pond_Demo.exe"), "export: the runtime is renamed after the project");
#else
        Check(fs::exists(out / "Pond_Demo"), "export: the runtime is renamed after the project");
#endif
        Check(fs::exists(out / "libUHE.so") && fs::exists(out / "libslang-compiler.so.0.2026.10") &&
                  fs::exists(out / "glfw3.dll"),
              "export: shared libraries ship");
        Check(!fs::exists(out / "libslang-llvm.so") && !fs::exists(out / "libslang-glslang-2026.10.so") &&
                  !fs::exists(out / "slang-glsl-module-2026.10.dll") && !fs::exists(out / "libgfx.so.0.2026.10"),
              "export: Slang's compile-only modules do not");
        Check(!fs::exists(out / "libslang-compiler.so"), "export: linker-name stand-ins do not");
        Check(!fs::exists(out / "uhe_shaderc"), "export: other tools in the build folder do not");
        Check(fs::exists(out / "assets/shaders/Basic3D.vertex.default.spv") &&
                  fs::exists(out / "assets/shaders/variants.txt") && fs::exists(out / "assets/model/hero.glb"),
              "export: engine assets ship, compiled shaders included");
        Check(!fs::exists(out / "assets/shaders/Basic3D.slang"), "export: shader sources do not ship");
        Check(!fs::exists(out / "assets/model/unused.glb") && !fs::exists(out / "Assets/Big/Unused.glb"),
              "export: unreferenced assets do not ship");
        Check(fs::exists(out / Project::kManifestName) && fs::exists(out / "Assets/Scenes/Pond.unhuman") &&
                  fs::exists(out / "Assets/Models/Ground Mesh.bin"),
              "export: the project's manifest and assets ship");
        Check(report.Warnings.size() == 3, "export: the collector's warnings reach the report");
        Check(fs::exists(out / GameExporter::kMarkerName), "export: the folder is marked as an export");

        report = GameExporter::Export(pond, options);
        Check(report.Ok, "export: exporting again over a previous export works");

        WriteFile(sandbox / "notes/todo.txt", "mine");
        options.OutputDir = sandbox / "notes";
        report = GameExporter::Export(pond, options);
        Check(!report.Ok && fs::exists(sandbox / "notes/todo.txt"),
              "export: a folder with someone's files in it is refused and left alone");

        options.OutputDir = gameRoot2 / "Build";
        report = GameExporter::Export(pond, options);
        Check(!report.Ok, "export: an output folder inside the project is refused");

        options.OutputDir = sandbox / "out_all";
        options.AllAssets = true;
        report = GameExporter::Export(pond, options);
        Check(report.Ok && fs::exists(options.OutputDir / "Assets/Big/Unused.glb") &&
                  fs::exists(options.OutputDir / "assets/model/unused.glb") &&
                  !fs::exists(options.OutputDir / "assets/shaders/Basic3D.slang"),
              "export: AllAssets copies every asset but shader sources");
        options.AllAssets = false;

        Check(GameExporter::ExecutableName("My Game: 2!") == "My_Game__2_", "export: names are made file-safe");
    }

    fs::remove_all(sandbox, ec);
    std::printf(g_failures == 0 ? "\nALL CHECKS PASSED\n" : "\n%d CHECK(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
