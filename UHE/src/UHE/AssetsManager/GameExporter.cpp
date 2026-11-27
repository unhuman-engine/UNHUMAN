#include "uhepch.h"
#include "UHE/AssetsManager/GameExporter.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <yaml-cpp/yaml.h>
#include "UHE/AssetsManager/AssetCatalog.h"
#include "UHE/AssetsManager/CookedFile.h"
#include "UHE/AssetsManager/GltfFiles.h"

namespace UHE
{

namespace fs = std::filesystem;

namespace
{
/// Engine asset files a game never reads: shader sources (the compiled .spv
/// ship instead) and the GLSL leftovers next to them.
bool IsShaderSource(const fs::path& file)
{
    const std::string extension = file.extension().string();
    return extension == ".slang" || extension == ".glsl";
}

/// A shared library the runtime may load: .so (also versioned, libfoo.so.1) or .dll.
bool IsSharedLibrary(const fs::path& file)
{
    const std::string name = file.filename().string();
    return file.extension() == ".dll" || name.ends_with(".so") || name.find(".so.") != std::string::npos;
}

/// Slang loads these only while compiling, and a game never compiles: LLVM
/// (150 MB), glslang, the GLSL module, its runtime and the gfx layer.
bool IsSlangCompilerModule(const fs::path& file)
{
    std::string name = file.filename().string();
    if (name.starts_with("lib"))
        name.erase(0, 3);
    for (const char* module : {"slang-llvm", "slang-glslang", "slang-glsl-module", "slang-rt", "gfx"})
    {
        const std::size_t length = std::strlen(module);
        if (name.compare(0, length, module) == 0 &&
            (name.size() == length || name[length] == '.' || name[length] == '-'))
            return true;
    }
    return false;
}

/// An ELF or PE image. The build folder also holds linker-name stand-ins
/// (libslang.so containing the text "libslang-compiler.so.0..."), which the
/// loader never opens.
bool IsBinary(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::array<char, 4> magic{};
    in.read(magic.data(), magic.size());
    return (in.gcount() == 4 && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') ||
           (in.gcount() >= 2 && magic[0] == 'M' && magic[1] == 'Z');
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// Extensions of the files content loads by name: a value with one of these
/// that resolves to nothing is reported, any other value is just not a path.
bool LooksLikeAsset(const fs::path& path)
{
    static const std::set<std::string> kExtensions = {".gltf", ".glb", ".fbx",  ".obj", ".png",     ".jpg", ".jpeg",
                                                      ".tga",  ".bmp", ".hdr",  ".ktx", ".ktx2",    ".dds", ".wav",
                                                      ".ogg",  ".mp3", ".flac", ".lua", ".unhuman", ".ttf", ".otf"};
    return kExtensions.contains(Lower(path.extension().string()));
}

/// The engine files the runtime opens by itself, whatever the scenes hold
/// (Renderer3D/2D, PostProcess, SSAO, Water: shaders; Scene: light icons;
/// Font and ImGuiLayer: Inter; WindowsWindow: the window icon).
constexpr std::array kEngineRuntimeFolders = {"shaders", "icon"};
constexpr std::array kEngineRuntimeFiles = {"Inter/static/Inter_18pt-Regular.ttf", "Inter/static/Inter_18pt-Bold.ttf"};

std::string ReadText(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool IsLicence(const fs::path& file)
{
    const std::string stem = Lower(file.stem().string());
    for (const char* prefix : {"license", "licence", "copying", "notice", "ofl"})
        if (stem.starts_with(prefix))
            return true;
    return false;
}

/// Walks the references from the project's scenes and scripts to every file they need.
class AssetCollector
{
public:
    AssetCollector(fs::path projectAssets, fs::path engineAssets)
        : m_Roots{std::move(projectAssets), std::move(engineAssets)}
    {
    }

    /// Also follows the references of a scene outside both roots (a start scene beside the manifest).
    void AddExternalScene(const fs::path& file) { m_ExternalScenes.push_back(file); }

    AssetManifest Run()
    {
        std::error_code ec;
        for (const char* folder : kEngineRuntimeFolders)
        {
            for (fs::recursive_directory_iterator it(m_Roots[kEngine] / folder, ec), end; it != end && !ec;
                 it.increment(ec))
            {
                if (it->is_regular_file(ec) && !IsShaderSource(it->path()))
                    Add(kEngine, it->path().lexically_relative(m_Roots[kEngine]));
            }
        }
        for (const char* file : kEngineRuntimeFiles)
            Add(kEngine, file);

        // Every scene and script: a game can load any of them by name.
        for (fs::recursive_directory_iterator it(m_Roots[kProject], ec), end; it != end && !ec; it.increment(ec))
        {
            const std::string extension = Lower(it->path().extension().string());
            if (it->is_regular_file(ec) && (extension == ".unhuman" || extension == ".lua"))
                Add(kProject, it->path().lexically_relative(m_Roots[kProject]));
        }

        for (const fs::path& scene : m_ExternalScenes)
        {
            try
            {
                Values(YAML::LoadFile(scene.string()), scene.filename(), false);
            }
            catch (const YAML::Exception& e)
            {
                Warn(scene.filename(), std::string("could not be read: ") + e.what());
            }
        }

        while (!m_Pending.empty())
        {
            const auto [root, file] = m_Pending.back();
            m_Pending.pop_back();
            Scan(root, file);
        }

        AssetManifest manifest;
        manifest.ProjectFiles.assign(m_Files[kProject].begin(), m_Files[kProject].end());
        manifest.EngineFiles.assign(m_Files[kEngine].begin(), m_Files[kEngine].end());
        manifest.Warnings = std::move(m_Warnings);
        // Every model, with the dependencies nothing but models uses.
        for (const int root : {kProject, kEngine})
        {
            auto& models = root == kProject ? manifest.ProjectModelFiles : manifest.EngineModelFiles;
            for (const fs::path& file : m_Files[root])
            {
                const std::string extension = Lower(file.extension().string());
                if (extension != ".gltf" && extension != ".glb")
                    continue;
                std::vector<fs::path>& only = models[file];
                for (const fs::path& dependency : m_ModelDependencies[root][file])
                {
                    if (!m_Direct[root].contains(dependency))
                        only.push_back(dependency);
                }
            }
        }
        return manifest;
    }

private:
    static constexpr int kProject = 0;
    static constexpr int kEngine = 1;

    /// Queues \p relative under root \p root (and the licences beside it) unless it is already in.
    /// \p fromModel: a glTF's buffer or image; anything else is used directly and
    /// ships even when every model is cooked.
    void Add(int root, const fs::path& path, bool fromModel = false)
    {
        // Relative to its root and inside it: the export writes it to out / root / path,
        // and a path climbing out (a symlink resolved to its target) would land outside.
        const std::optional<fs::path> inside = Inside(path);
        if (!inside)
        {
            Warn(path, "lies outside the asset folder and was not exported");
            return;
        }
        const fs::path relative = *inside;
        std::error_code ec;
        if (!fromModel)
            m_Direct[root].insert(relative);
        if (!fs::is_regular_file(m_Roots[root] / relative, ec) || !m_Files[root].insert(relative).second)
            return;
        m_Pending.emplace_back(root, relative);
        for (fs::path folder = relative.parent_path();; folder = folder.parent_path())
        {
            if (m_LicenceFolders[root].insert(folder).second)
            {
                for (const fs::directory_entry& entry : fs::directory_iterator(m_Roots[root] / folder, ec))
                {
                    if (entry.is_regular_file(ec) && IsLicence(entry.path()))
                        Add(root, folder / entry.path().filename());
                }
            }
            if (folder.empty())
                break;
        }
    }

    /// A path that stays inside its root, or nothing when it climbs out of it.
    static std::optional<fs::path> Inside(const fs::path& path)
    {
        const fs::path normal = path.lexically_normal();
        if (normal.empty() || normal.is_absolute() || normal.has_root_name() || *normal.begin() == "..")
            return std::nullopt;
        return normal;
    }

    /// Resolves a name the way ResolveAssetPath does at run time; adds the file when it exists.
    /// \p reportMissing: the value is meant to be a path, so one that looks
    /// like an asset and resolves to nothing is a warning.
    void Reference(const std::string& value, const fs::path& from, bool reportMissing = true)
    {
        std::string name = value;
        int only = -1;
        if (name.starts_with("game:"))
            only = kProject, name.erase(0, 5);
        else if (name.starts_with("engine:"))
            only = kEngine, name.erase(0, 7);
        if (only >= 0)
            name.erase(0, name.find_first_not_of("/\\"));
        if (name.empty() || name.size() > 512 || name.find('\n') != std::string::npos)
            return;

        const fs::path path(name);
        std::error_code ec;
        if (path.is_absolute() || path.has_root_name())
        {
            if (reportMissing && (fs::is_regular_file(path, ec) || LooksLikeAsset(path)))
                Warn(from, "'" + name +
                               "' is an absolute path, which the game's machine will not have; use a path "
                               "relative to Assets/");
            return;
        }
        const std::optional<fs::path> relative = Inside(path);
        if (!relative)
            return;
        for (const int root : {kProject, kEngine})
        {
            if ((only < 0 || only == root) && fs::is_regular_file(m_Roots[root] / *relative, ec))
            {
                Add(root, *relative);
                return;
            }
        }
        if (reportMissing && LooksLikeAsset(path))
            Warn(from, "'" + name + "' is not in the project's or the engine's assets");
    }

    void Scan(int root, const fs::path& relative)
    {
        const fs::path file = m_Roots[root] / relative;
        const std::string extension = Lower(relative.extension().string());
        if (extension == ".unhuman" || extension == ".uheproject")
        {
            try
            {
                Values(YAML::LoadFile(file.string()), relative, false);
            }
            catch (const YAML::Exception& e)
            {
                Warn(relative, std::string("could not be read: ") + e.what());
            }
        }
        else if (extension == ".lua")
        {
            static const std::regex kString(R"lua("([^"\\\n]*)"|'([^'\\\n]*)')lua");
            const std::string text = ReadText(file);
            for (std::sregex_iterator it(text.begin(), text.end(), kString), end; it != end; ++it)
                Reference((*it)[1].matched ? (*it)[1].str() : (*it)[2].str(), relative);
        }
        else if (extension == ".gltf" || extension == ".glb")
        {
            // Buffers and images beside the model, relative to its folder.
            const std::optional<std::vector<fs::path>> files = GltfExternalFiles(file);
            if (!files)
            {
                Warn(relative, "is not a valid glTF binary (truncated?); its dependencies were not followed");
                return;
            }
            for (const fs::path& uri : *files)
            {
                const std::optional<fs::path> dependency = Inside(relative.parent_path() / uri);
                std::error_code ec;
                if (dependency && fs::is_regular_file(m_Roots[root] / *dependency, ec))
                {
                    Add(root, *dependency, true);
                    m_ModelDependencies[root][relative].push_back(*dependency);
                }
                else
                    Warn(relative, "points at '" + uri.generic_string() + "', which is missing");
            }
        }
    }

    /// Every scalar in a YAML document; map keys are field names, not paths.
    /// Any value that resolves is kept; only a value under a "...Path" key
    /// (ModelPath, TexturePath, ScriptPath, ...) is reported when it does not,
    /// so a tag named "Ground.gltf" is not a warning.
    void Values(const YAML::Node& node, const fs::path& from, bool isPath)
    {
        if (node.IsScalar())
            Reference(node.Scalar(), from, isPath);
        else if (node.IsSequence())
            for (const YAML::Node& child : node)
                Values(child, from, isPath);
        else if (node.IsMap())
            for (const auto& entry : node)
                Values(entry.second, from, entry.first.IsScalar() && entry.first.Scalar().ends_with("Path"));
    }

    void Warn(const fs::path& from, const std::string& message)
    {
        m_Warnings.push_back(from.generic_string() + ": " + message);
    }

    std::array<fs::path, 2> m_Roots;
    std::array<std::set<fs::path>, 2> m_Files;
    std::array<std::set<fs::path>, 2> m_LicenceFolders;
    std::vector<std::pair<int, fs::path>> m_Pending;
    std::set<fs::path> m_Direct[2]; ///< referenced by something other than a model
    std::map<fs::path, std::vector<fs::path>> m_ModelDependencies[2];
    std::vector<fs::path> m_ExternalScenes;
    std::vector<std::string> m_Warnings;
};

class Copier
{
public:
    explicit Copier(ExportReport& report) : m_Report(report) {}

    bool File(const fs::path& from, const fs::path& to)
    {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            m_Report.Errors.push_back("could not copy '" + from.string() + "': " + ec.message());
            return false;
        }
        ++m_Report.FilesCopied;
        m_Report.BytesCopied += fs::file_size(to, ec);
        return true;
    }

    /// Copies every regular file under \p from into \p to, keeping the layout; \p skip filters files out.
    template <class Skip> void Tree(const fs::path& from, const fs::path& to, Skip&& skip)
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec))
        {
            if (!it->is_regular_file(ec) || skip(it->path()))
                continue;
            // Lexical, so a symlink keeps its own place in the tree instead of
            // its target's, and the destination stays inside the output.
            const fs::path relative = it->path().lexically_relative(from).lexically_normal();
            if (relative.empty() || *relative.begin() == "..")
            {
                m_Report.Warnings.push_back("'" + it->path().string() + "' lies outside '" + from.string() +
                                            "' and was not exported");
                continue;
            }
            File(it->path(), to / relative);
        }
        if (ec)
            m_Report.Errors.push_back("could not read '" + from.string() + "': " + ec.message());
    }

private:
    ExportReport& m_Report;
};
} // namespace

namespace
{
/// Every file under \p root, relative and generic, with its hash; generated files left out.
std::map<std::string, u64> HashTree(const fs::path& root)
{
    std::map<std::string, u64> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        const std::string extension = Lower(it->path().extension().string());
        const std::string name = it->path().filename().string();
        // Cooks and temporaries are made from other files; dot files are the OS's or an editor's.
        if (extension == ".uhemodel" || extension == ".tmp" || name.starts_with("."))
            continue;
        files[it->path().lexically_relative(root).generic_string()] = Cooked::HashFile(it->path());
    }
    return files;
}
} // namespace

void GameExporter::WriteAssetManifest(const ProjectInfo& project, const ExportOptions& options, const fs::path& out,
                                      ExportReport& report)
{
    // The project's registry: one ID per file, kept across exports and moves.
    AssetCatalog registry;
    const fs::path registryFile = project.RootPath / AssetCatalog::kRegistryName;
    std::string error;
    if (!registry.Load(registryFile, &error))
        report.Warnings.push_back(error + "; asset IDs start over");
    if (options.UpdateRegistry && registry.Assign(HashTree(project.AssetPath)) && !registry.Save(registryFile, &error))
        report.Warnings.push_back("asset IDs were not saved: " + error);

    // What shipped, as the game finds it.
    AssetCatalog manifest;
    manifest.Platform = options.Platform;
    const auto add = [&](const std::string& rootName, const fs::path& root)
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            if (!it->is_regular_file(ec) || it->path().filename() == AssetCatalog::kManifestName)
                continue;
            AssetCatalogEntry entry;
            entry.Root = rootName;
            const std::string stored = it->path().lexically_relative(root).generic_string();
            entry.Path = stored;
            if (Lower(it->path().extension().string()) == ".uhemodel")
            {
                entry.Path = stored.substr(0, stored.size() - std::string(".uhemodel").size());
                entry.Stored = stored;
            }
            entry.Type = AssetCatalog::TypeOf(entry.Path);
            entry.Size = static_cast<u64>(it->file_size(ec));
            entry.Hash = Cooked::HashFile(it->path());
            const AssetID known = rootName == "project" ? registry.IdFor(entry.Path) : AssetID(0);
            entry.ID = static_cast<u64>(known) != 0 ? known : AssetCatalog::DerivedID(rootName, entry.Path);
            manifest.Add(std::move(entry));
        }
    };
    add("project", out / Project::kAssetFolderName);
    add("engine", out / "assets");

    // A glTF that shipped as it is reads its buffers and images: those are its dependencies.
    std::vector<AssetCatalogEntry> entries = manifest.Entries();
    for (AssetCatalogEntry& entry : entries)
    {
        if (!entry.Stored.empty() || entry.Type != "model")
            continue;
        const fs::path folder = entry.Root == "project" ? out / Project::kAssetFolderName : out / "assets";
        if (const auto files = GltfExternalFiles(folder / entry.Path))
            for (const fs::path& file : *files)
                if (const AssetCatalogEntry* dependency = manifest.FindByPath(
                        (fs::path(entry.Path).parent_path() / file).lexically_normal().generic_string()))
                    entry.Dependencies.push_back(dependency->ID);
    }
    AssetCatalog finished;
    finished.Platform = manifest.Platform;
    for (AssetCatalogEntry& entry : entries)
        finished.Add(std::move(entry));
    if (!finished.Save(out / Project::kAssetFolderName / AssetCatalog::kManifestName, &error))
        report.Errors.push_back("could not write the asset manifest: " + error);
    else
        ++report.FilesCopied;
}

AssetManifest GameExporter::CollectAssets(const fs::path& projectAssets, const fs::path& engineAssets,
                                          const std::vector<fs::path>& extraScenes)
{
    AssetCollector collector(projectAssets, engineAssets);
    for (const fs::path& scene : extraScenes)
        collector.AddExternalScene(scene);
    return collector.Run();
}

std::vector<std::string> GameExporter::MissingShaderArtifacts(const fs::path& engineAssets)
{
    // A game ships no shader sources and cannot compile: each .slang needs at
    // least one compiled stage beside it (<stem>.<stage>.<variant>.spv, ShaderArtifactPath).
    // A folder with a shaders.manifest (written by uhe_shaderc) needs that only
    // for the sources it lists: the rest are libraries and work in progress.
    std::vector<std::string> missing;
    std::error_code ec;
    std::map<fs::path, std::set<std::string>> manifests; // folder -> sources it lists
    for (fs::recursive_directory_iterator it(engineAssets, ec), end; it != end && !ec; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || it->path().filename() != kShaderManifestName)
            continue;
        std::set<std::string>& listed = manifests[it->path().parent_path()];
        std::ifstream in(it->path());
        for (std::string line; std::getline(in, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                listed.insert(line);
        }
    }
    const auto required = [&](const fs::path& source)
    {
        for (const auto& [folder, listed] : manifests)
        {
            const fs::path relative = source.lexically_relative(folder);
            if (!relative.empty() && *relative.begin() != "..")
                return listed.count(relative.generic_string()) > 0;
        }
        return true; // no manifest covers it: every source counts
    };
    ec.clear();
    for (fs::recursive_directory_iterator it(engineAssets, ec), end; it != end && !ec; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || Lower(it->path().extension().string()) != ".slang" || !required(it->path()))
            continue;
        const std::string prefix = it->path().stem().string() + ".";
        bool compiled = false;
        for (const fs::directory_entry& sibling : fs::directory_iterator(it->path().parent_path(), ec))
        {
            const std::string name = sibling.path().filename().string();
            if (name.starts_with(prefix) && name.ends_with(".spv"))
            {
                compiled = true;
                break;
            }
        }
        if (!compiled)
            missing.push_back(it->path().lexically_relative(engineAssets).generic_string());
    }
    std::sort(missing.begin(), missing.end());
    return missing;
}

std::string GameExporter::ExecutableName(const std::string& projectName)
{
    std::string name;
    for (const char c : projectName)
    {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                          c == '_' || c == '.';
        name += safe ? c : '_';
    }
    return name.empty() || name == "." || name == ".." ? std::string("Game") : name;
}

ExportReport GameExporter::Export(const ProjectInfo& project, const ExportOptions& options)
{
    ExportReport report;
    std::error_code ec;
    const auto fail = [&](std::string message)
    {
        report.Errors.push_back(std::move(message));
        return report;
    };

    // ── What has to exist before anything is written ──
    if (!project.IsValid())
        return fail("no project is open");
    const fs::path startScene = Project::StartScenePath(project);
    if (startScene.empty() || !fs::is_regular_file(startScene, ec))
        return fail("the project has no start scene; set StartScene in Project.uheproject");
    // Copied to the same place relative to the manifest, so it must stay inside the project.
    const fs::path startRelative = fs::path(project.StartScene).lexically_normal();
    if (startRelative.is_absolute() || startRelative.has_root_name() || startRelative.empty() ||
        *startRelative.begin() == "..")
        return fail("the start scene '" + project.StartScene + "' is outside the project folder");
#ifdef _WIN32
    const std::string runtimeFile = options.RuntimeName + ".exe";
    const std::string gameFile = ExecutableName(project.Name) + ".exe";
#else
    const std::string runtimeFile = options.RuntimeName;
    const std::string gameFile = ExecutableName(project.Name);
#endif
    const fs::path runtime = options.RuntimeDir / runtimeFile;
    if (!fs::is_regular_file(runtime, ec))
        return fail("the runtime '" + runtime.string() + "' is missing; build the UHE_RUNTIME target");
    if (!fs::is_directory(options.EngineAssets, ec))
        return fail("the engine assets '" + options.EngineAssets.string() + "' are missing");
    if (options.OutputDir.empty())
        return fail("no output folder");
    if (const std::vector<std::string> missing = MissingShaderArtifacts(options.EngineAssets); !missing.empty())
    {
        std::string list;
        for (const std::string& shader : missing)
            list += (list.empty() ? "" : ", ") + shader;
        return fail("no compiled shader for " + list + "; build the uhe_shaders target first");
    }

    // Never write into a folder of the user's that was not an export.
    const fs::path out = fs::absolute(options.OutputDir, ec);
    if (fs::exists(out, ec))
    {
        if (!fs::is_directory(out, ec))
            return fail("'" + out.string() + "' is a file, not a folder");
        const bool empty = fs::directory_iterator(out, ec) == fs::directory_iterator();
        if (!empty && !fs::exists(out / kMarkerName, ec))
            return fail("'" + out.string() +
                        "' already has files in it and is not a previous export; choose an "
                        "empty folder");
    }
    const fs::path projectRoot = fs::weakly_canonical(project.RootPath, ec);
    const fs::path outCanonical = fs::weakly_canonical(out, ec);
    if (outCanonical.string().starts_with(projectRoot.string() + std::string(1, fs::path::preferred_separator)) ||
        outCanonical == projectRoot)
        return fail("the output folder is inside the project; it would export itself");

    fs::create_directories(out, ec);
    if (ec)
        return fail("could not create '" + out.string() + "': " + ec.message());

    Copier copy(report);

    // ── The runtime and its libraries ──
    copy.File(runtime, out / gameFile);
    fs::permissions(out / gameFile, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    for (const fs::directory_entry& entry : fs::directory_iterator(options.RuntimeDir, ec))
    {
        if (entry.is_regular_file(ec) && IsSharedLibrary(entry.path()) && !IsSlangCompilerModule(entry.path()) &&
            IsBinary(entry.path()))
            copy.File(entry.path(), out / entry.path().filename());
    }
    // The licenses of the libraries above travel with them: a packaged editor
    // keeps THIRD_PARTY_NOTICES.txt beside the runtime (script/package.py).
    if (const fs::path notices = options.RuntimeDir / "THIRD_PARTY_NOTICES.txt"; fs::is_regular_file(notices, ec))
        copy.File(notices, out / notices.filename());

    // ── The manifest: the settings being exported, which may not be saved yet ──
    {
        ProjectInfo exported = project;
        exported.RootPath = out;
        exported.AssetPath = out / Project::kAssetFolderName;
        std::string error;
        if (!Project::Save(exported, &error))
            report.Errors.push_back("could not write the manifest: " + error);
        else
            ++report.FilesCopied;
    }
    // A start scene beside the manifest rather than in Assets/ goes to the same place.
    const fs::path startInAssets = startScene.lexically_relative(project.AssetPath);
    const bool startOutsideAssets = startInAssets.empty() || *startInAssets.begin() == "..";
    if (startOutsideAssets)
        copy.File(startScene, out / startRelative);

    // ── Assets: what the game references, or everything ──
    if (options.AllAssets)
    {
        copy.Tree(options.EngineAssets, out / "assets", [](const fs::path& file) { return IsShaderSource(file); });
        copy.Tree(project.AssetPath, out / Project::kAssetFolderName, [](const fs::path&) { return false; });
    }
    else
    {
        AssetManifest manifest =
            CollectAssets(project.AssetPath, options.EngineAssets,
                          startOutsideAssets ? std::vector<fs::path>{startScene} : std::vector<fs::path>{});
        report.Warnings = std::move(manifest.Warnings);
        // Models ship cooked: the cook replaces the glTF and the files only
        // models use, unless a model that also uses one could not be cooked.
        std::set<fs::path> skipped[2];
        if (options.CookModel)
        {
            const auto cookRoot = [&](int root, const std::map<fs::path, std::vector<fs::path>>& models,
                                      const fs::path& from, const fs::path& to)
            {
                std::map<fs::path, int> uncookedUsers;
                std::set<fs::path> cooked;
                for (const auto& [model, files] : models)
                {
                    // Model::CookedPathFor: the source name plus ".uhemodel".
                    const fs::path target = to / (model.string() + ".uhemodel");
                    std::error_code ec;
                    fs::create_directories(target.parent_path(), ec);
                    std::string why;
                    if (options.CookModel(from / model, target, why))
                    {
                        cooked.insert(model);
                        ++report.ModelsCooked;
                        report.BytesCopied += fs::file_size(target, ec);
                    }
                    else
                    {
                        report.Warnings.push_back(model.generic_string() + " was not cooked (" + why +
                                                  "); it ships as glTF");
                        for (const fs::path& file : files)
                            ++uncookedUsers[file];
                    }
                }
                for (const auto& [model, files] : models)
                {
                    if (!cooked.contains(model))
                        continue;
                    skipped[root].insert(model);
                    for (const fs::path& file : files)
                    {
                        if (uncookedUsers[file] == 0)
                            skipped[root].insert(file);
                    }
                }
            };
            cookRoot(0, manifest.ProjectModelFiles, project.AssetPath, out / Project::kAssetFolderName);
            cookRoot(1, manifest.EngineModelFiles, options.EngineAssets, out / "assets");
        }
        for (const fs::path& file : manifest.EngineFiles)
        {
            if (!skipped[1].contains(file))
                copy.File(options.EngineAssets / file, out / "assets" / file);
        }
        for (const fs::path& file : manifest.ProjectFiles)
        {
            if (!skipped[0].contains(file))
                copy.File(project.AssetPath / file, out / Project::kAssetFolderName / file);
        }
    }

    WriteAssetManifest(project, options, out, report);

    {
        std::ofstream marker(out / kMarkerName);
        marker << "UHE game export of '" << project.Name << "'\n";
    }

    report.Ok = report.Errors.empty();
    report.Executable = out / gameFile;
    return report;
}

} // namespace UHE
