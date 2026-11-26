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

/// The JSON a glTF file holds: the whole .gltf, or the first chunk of a .glb;
/// nothing for a .glb whose header does not fit the file (truncated or not a glTF).
std::optional<std::string> GltfJson(const fs::path& file)
{
    if (Lower(file.extension().string()) != ".glb")
        return ReadText(file);
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    std::ifstream in(file, std::ios::binary);
    std::array<std::uint32_t, 5> header{}; // magic, version, length, chunk length, chunk type
    in.read(reinterpret_cast<char*>(header.data()), sizeof(header));
    // The lengths come from the file: checked against its real size before
    // anything is allocated, so a broken header cannot ask for 4 GB.
    if (!in || ec || header[0] != 0x46546C67u || header[4] != 0x4E4F534Au || header[2] > size ||
        std::uintmax_t{header[3]} > size - std::min<std::uintmax_t>(size, sizeof(header)))
        return std::nullopt;
    std::string json(header[3], '\0');
    in.read(json.data(), static_cast<std::streamsize>(json.size()));
    if (!in)
        return std::nullopt;
    return json;
}

/// "%20" and friends in a glTF URI, decoded to the file name on disk.
std::string DecodeUri(const std::string& uri)
{
    std::string out;
    for (std::size_t i = 0; i < uri.size(); ++i)
    {
        if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(uri[i + 2])))
        {
            out += static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else
            out += uri[i];
    }
    return out;
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
        return manifest;
    }

private:
    static constexpr int kProject = 0;
    static constexpr int kEngine = 1;

    /// Queues \p relative under root \p root (and the licences beside it) unless it is already in.
    void Add(int root, const fs::path& path)
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
            static const std::regex kUri(R"json("uri"\s*:\s*"([^"]*)")json");
            const std::optional<std::string> parsed = GltfJson(file);
            if (!parsed)
            {
                Warn(relative, "is not a valid glTF binary (truncated?); its dependencies were not followed");
                return;
            }
            const std::string& json = *parsed;
            for (std::sregex_iterator it(json.begin(), json.end(), kUri), end; it != end; ++it)
            {
                const std::string uri = (*it)[1].str();
                if (uri.starts_with("data:"))
                    continue;
                const std::optional<fs::path> dependency = Inside(relative.parent_path() / DecodeUri(uri));
                std::error_code ec;
                if (dependency && fs::is_regular_file(m_Roots[root] / *dependency, ec))
                    Add(root, *dependency);
                else
                    Warn(relative, "points at '" + uri + "', which is missing");
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
        for (const fs::path& file : manifest.EngineFiles)
            copy.File(options.EngineAssets / file, out / "assets" / file);
        for (const fs::path& file : manifest.ProjectFiles)
            copy.File(project.AssetPath / file, out / Project::kAssetFolderName / file);
        report.Warnings = std::move(manifest.Warnings);
    }

    {
        std::ofstream marker(out / kMarkerName);
        marker << "UHE game export of '" << project.Name << "'\n";
    }

    report.Ok = report.Errors.empty();
    report.Executable = out / gameFile;
    return report;
}

} // namespace UHE
