#include "uhepch.h"
#include "Project.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>
#include <system_error>
#include <yaml-cpp/yaml.h>
#include "UHE/AssetsManager/VfsSystem.h"

namespace UHE
{
namespace
{
void SetError(std::string* error, const std::string& message)
{
    if (error != nullptr)
        *error = message;
}

std::string EngineVersionString()
{
    return "0.1";
}
} // namespace

std::filesystem::path Project::ManifestPath(const std::filesystem::path& path)
{
    if (path.empty())
        return {};
    if (path.filename() == kManifestName)
        return path;
    return path / kManifestName;
}

bool Project::IsProject(const std::filesystem::path& path)
{
    std::error_code ec;
    const std::filesystem::path manifest = ManifestPath(path);
    return !manifest.empty() && std::filesystem::is_regular_file(manifest, ec);
}

std::filesystem::path Project::NormalizeRoot(const std::filesystem::path& path)
{
    if (path.empty())
        return {};
    std::filesystem::path root = path;
    if (root.filename() == kManifestName)
        root = root.parent_path();
    // A bare "Project.uheproject" names the folder we are in.
    if (root.empty())
        root = ".";
    std::error_code ec;
    root = std::filesystem::absolute(root, ec).lexically_normal();
    // lexically_normal keeps a trailing separator ("a/b/"): drop it, so the
    // two spellings of a folder compare equal.
    if (!root.has_filename() && root.has_relative_path())
        root = root.parent_path();
    return root;
}

std::string Project::CheckNewProject(const std::filesystem::path& parent, const std::string& name)
{
    if (name.empty())
        return "Enter a name";
    if (name == "." || name == "..")
        return "'" + name + "' is not a folder name";
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.')
        return "The name cannot start or end with a space, or end with a dot";
    for (const char c : name)
    {
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 32)
            return std::string("The name cannot contain '") + c + "'";
    }
    if (parent.empty())
        return "Choose a folder to create the project in";

    std::error_code ec;
    const std::filesystem::path root = NormalizeRoot(parent / name);
    if (std::filesystem::exists(root, ec))
    {
        if (!std::filesystem::is_directory(root, ec))
            return "'" + root.string() + "' is a file";
        if (IsProject(root))
            return "'" + root.string() + "' is already a project; open it instead";
        if (std::filesystem::directory_iterator(root, ec) != std::filesystem::directory_iterator())
            return "'" + root.string() + "' already has files in it";
    }
    return {};
}

bool Project::EnsureAssetFolder(const ProjectInfo& info, std::string* error)
{
    if (info.RootPath.empty())
    {
        SetError(error, "project has no root path");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(info.RootPath / kAssetFolderName, ec);
    if (ec)
    {
        SetError(error, "could not create '" + (info.RootPath / kAssetFolderName).string() + "': " + ec.message());
        return false;
    }
    return true;
}

bool Project::Create(const std::filesystem::path& root, const std::string& name, ProjectInfo& out, std::string* error)
{
    if (name.empty())
    {
        SetError(error, "project name must not be empty");
        return false;
    }
    if (root.empty())
    {
        SetError(error, "project folder must not be empty");
        return false;
    }

    std::error_code ec;
    const std::filesystem::path absoluteRoot = NormalizeRoot(root);
    const std::filesystem::path manifest = ManifestPath(absoluteRoot);

    if (std::filesystem::exists(manifest, ec))
    {
        SetError(error, "'" + manifest.string() + "' already exists");
        return false;
    }

    std::filesystem::create_directories(absoluteRoot / kAssetFolderName, ec);
    if (ec)
    {
        SetError(error, "could not create the project folder: " + ec.message());
        return false;
    }

    ProjectInfo info;
    info.Name = name;
    info.RootPath = absoluteRoot;
    info.AssetPath = absoluteRoot / kAssetFolderName;
    info.EngineVersion = EngineVersionString();

    if (!Save(info, error))
        return false;

    out = std::move(info);
    return true;
}

bool Project::Load(const std::filesystem::path& path, ProjectInfo& out, std::string* error)
{
    const std::filesystem::path manifest = ManifestPath(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifest, ec))
    {
        SetError(error, "no '" + std::string(kManifestName) + "' found at '" + manifest.string() + "'");
        return false;
    }

    YAML::Node root;
    try
    {
        root = YAML::LoadFile(manifest.string());
    }
    catch (const std::exception& e)
    {
        SetError(error, std::string("could not read '") + manifest.string() + "': " + e.what());
        return false;
    }

    const YAML::Node project = root["Project"];
    if (!project || !project.IsMap())
    {
        SetError(error, "'" + manifest.string() + "' has no 'Project' section");
        return false;
    }

    ProjectInfo info;
    // Absolute: a relative root ("projects/Snake" from -project) would follow
    // the working directory and end up in the recent list that way.
    info.RootPath = NormalizeRoot(manifest);
    info.AssetPath = info.RootPath / kAssetFolderName;
    // A manifest without a name still opens: fall back to the folder name.
    info.Name = project["Name"] ? project["Name"].as<std::string>() : info.RootPath.filename().string();
    info.StartScene = project["StartScene"] ? project["StartScene"].as<std::string>() : std::string();
    info.EngineVersion = project["EngineVersion"] ? project["EngineVersion"].as<std::string>() : std::string();

    // The game window. A missing block or field keeps its default; sizes are
    // clamped, so a typo cannot open a 0 or 100000 pixel window.
    if (const YAML::Node window = project["Window"]; window && window.IsMap())
    {
        ProjectWindowSettings& settings = info.Window;
        const auto size = [&](const char* key, u32 fallback)
        {
            const i64 value = window[key] ? window[key].as<i64>(fallback) : fallback;
            return static_cast<u32>(
                std::clamp<i64>(value, ProjectWindowSettings::kMinSize, ProjectWindowSettings::kMaxSize));
        };
        settings.Title = window["Title"] ? window["Title"].as<std::string>(std::string()) : std::string();
        settings.Width = size("Width", settings.Width);
        settings.Height = size("Height", settings.Height);
        settings.Mode = window["Mode"] ? ParseWindowMode(window["Mode"].as<std::string>(std::string()), settings.Mode)
                                       : settings.Mode;
        settings.VSync = window["VSync"] ? window["VSync"].as<bool>(settings.VSync) : settings.VSync;
    }

    if (info.Name.empty())
        info.Name = info.RootPath.filename().string();

    out = std::move(info);
    return true;
}

bool Project::Save(const ProjectInfo& info, std::string* error)
{
    if (info.RootPath.empty())
    {
        SetError(error, "project has no root path");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(info.RootPath, ec);
    if (ec)
    {
        SetError(error, "could not create '" + info.RootPath.string() + "': " + ec.message());
        return false;
    }

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "Project" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "Name" << YAML::Value << info.Name;
    out << YAML::Key << "EngineVersion" << YAML::Value
        << (info.EngineVersion.empty() ? EngineVersionString() : info.EngineVersion);
    out << YAML::Key << "StartScene" << YAML::Value << info.StartScene;
    out << YAML::Key << "Window" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "Title" << YAML::Value << info.Window.Title;
    out << YAML::Key << "Width" << YAML::Value << info.Window.Width;
    out << YAML::Key << "Height" << YAML::Value << info.Window.Height;
    out << YAML::Key << "Mode" << YAML::Value << WindowModeName(info.Window.Mode);
    out << YAML::Key << "VSync" << YAML::Value << info.Window.VSync;
    out << YAML::EndMap;
    out << YAML::EndMap;
    out << YAML::EndMap;

    const std::filesystem::path manifest = ManifestPath(info.RootPath);
    std::ofstream file(manifest);
    if (!file)
    {
        SetError(error, "could not write '" + manifest.string() + "'");
        return false;
    }
    file << out.c_str();
    if (!file.good())
    {
        SetError(error, "could not write '" + manifest.string() + "'");
        return false;
    }
    return true;
}

void Project::Mount(const ProjectInfo& info, const std::filesystem::path& engineAssetPath)
{
    if (!info.AssetPath.empty())
        Vfs::Get().Mount("game:", info.AssetPath);
    if (!engineAssetPath.empty())
        Vfs::Get().Mount("engine:", engineAssetPath);
}

const char* WindowModeName(WindowMode mode)
{
    switch (mode)
    {
        case WindowMode::Borderless:
            return "Borderless";
        case WindowMode::Fullscreen:
            return "Fullscreen";
        case WindowMode::Windowed:
            break;
    }
    return "Windowed";
}

WindowMode ParseWindowMode(const std::string& name, WindowMode fallback)
{
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const WindowMode mode : {WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen})
    {
        std::string candidate = WindowModeName(mode);
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == candidate)
            return mode;
    }
    return fallback;
}

std::filesystem::path Project::StartScenePath(const ProjectInfo& info)
{
    if (info.StartScene.empty() || info.RootPath.empty())
        return {};
    return (info.RootPath / info.StartScene).lexically_normal();
}

std::size_t Project::ScanFolder(const std::filesystem::path& folder, std::vector<ProjectInfo>& out)
{
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec))
        return 0;

    auto canonicalKey = [](const std::filesystem::path& path)
    {
        std::error_code keyEc;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, keyEc);
        return keyEc ? path.lexically_normal() : canonical;
    };

    std::size_t added = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder, ec))
    {
        if (ec)
            break;
        if (!entry.is_directory())
            continue;

        ProjectInfo info;
        if (!Project::Load(entry.path(), info))
            continue;

        const std::filesystem::path key = canonicalKey(info.RootPath);
        const bool duplicate = std::any_of(out.begin(), out.end(), [&](const ProjectInfo& other)
                                           { return canonicalKey(other.RootPath) == key; });
        if (duplicate)
            continue;

        out.push_back(std::move(info));
        ++added;
    }
    return added;
}
} // namespace UHE
