#include "uhepch.h"
#include "UHE/AssetsManager/GameExporter.h"
#include <fstream>

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
            File(it->path(), to / fs::relative(it->path(), from, ec));
        }
        if (ec)
            m_Report.Errors.push_back("could not read '" + from.string() + "': " + ec.message());
    }

private:
    ExportReport& m_Report;
};
} // namespace

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
        if (entry.is_regular_file(ec) && IsSharedLibrary(entry.path()))
            copy.File(entry.path(), out / entry.path().filename());
    }

    // ── Engine assets, without shader sources ──
    copy.Tree(options.EngineAssets, out / "assets", [](const fs::path& file) { return IsShaderSource(file); });

    // ── The project ──
    copy.File(Project::ManifestPath(project.RootPath), out / Project::kManifestName);
    copy.Tree(project.AssetPath, out / Project::kAssetFolderName, [](const fs::path&) { return false; });

    {
        std::ofstream marker(out / kMarkerName);
        marker << "UHE game export of '" << project.Name << "'\n";
    }

    report.Ok = report.Errors.empty();
    report.Executable = out / gameFile;
    return report;
}

} // namespace UHE
