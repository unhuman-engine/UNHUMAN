#include "VfsSystem.h"
#include "uhepch.h"
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace UHE {

// ---- Vfs -------------------------------------------------------------------

void Vfs::Mount(std::string_view virtualRoot, const fs::path& physicalRoot)
{
    std::string root = std::string(virtualRoot);
    // Normalize: a trailing separator is implicit.
    while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
        root.pop_back();

    fs::path physical = fs::absolute(physicalRoot).lexically_normal();
    auto it = std::find_if(m_Mounts.begin(), m_Mounts.end(),
                           [&](const auto& mount) { return mount.first == root; });
    if (it != m_Mounts.end())
        it->second = physical;
    else
        m_Mounts.emplace_back(std::move(root), physical);
}

std::optional<fs::path> Vfs::Resolve(std::string_view virtualPath) const
{
    auto results = ResolveAll(virtualPath);
    if (results.empty())
        return std::nullopt;
    return results.back();
}

std::vector<fs::path> Vfs::ResolveAll(std::string_view virtualPath) const
{
    std::vector<fs::path> results;
    for (const auto& [root, physical] : m_Mounts)
    {
        std::string_view prefix = virtualPath;
        if (!prefix.starts_with(root))
            continue;
        std::string_view remainder = prefix.substr(root.size());
        // Boundary check: for roots not ending in ':', require a path separator or end-of-string
        if (!root.empty() && root.back() != ':' && !remainder.empty() &&
            remainder.front() != '/' && remainder.front() != '\\')
        {
            continue;
        }
        while (!remainder.empty() && (remainder.front() == '/' || remainder.front() == '\\'))
            remainder.remove_prefix(1);
        fs::path resolved = (physical / fs::path(remainder)).lexically_normal();
        // Traversal guard: reject paths that escape the physical mount root
        auto [rootEnd, _] = std::mismatch(physical.begin(), physical.end(), resolved.begin(), resolved.end());
        if (rootEnd != physical.end())
            continue;
        results.push_back(resolved.make_preferred());
    }
    return results;
}

// ---- legacy FileSystem shim -------------------------------------------------

void FileSystem::Initialize(const char *argv0) {
  fs::path exePath = fs::absolute(argv0).parent_path();
  bool foundSourceAssets = false;

  // Unified asset discovery: prefer the marker matching the running binary,
  // falling back to other project markers if target-specific assets are absent.
  std::string_view exeName = argv0 ? argv0 : "";
  bool preferGame = exeName.find("UHEGAME") != std::string_view::npos;
  const char *kProjectMarkers[] = {
      preferGame ? "UHEGAME" : "UHE_EDITOR",
      preferGame ? "UHE_EDITOR" : "UHEGAME"
  };
  fs::path searchPath = exePath;
  while (searchPath.has_parent_path()) {
    for (const char *marker : kProjectMarkers) {
      if (fs::exists(searchPath / marker / "assets")) {
        m_RootPath = searchPath / marker;
        foundSourceAssets = true;
        break;
      }
    }
    if (foundSourceAssets)
      break;

    fs::path parent = searchPath.parent_path();
    if (parent == searchPath)
      break;
    searchPath = parent;
  }

  if (!foundSourceAssets) {
    m_RootPath = exePath;
    if (!fs::exists(m_RootPath / "assets")) {
      std::cerr << "[UHE::FileSystem] WARNING: 'assets' folder not found at: "
                << m_RootPath.string() << std::endl;
    }
  }

  // Mount the discovered root so Vfs::Resolve("game:/...") works everywhere.
  Vfs::Get().Mount("game:", m_RootPath / "assets");
}

std::string FileSystem::Resolve(const std::string &virtualPath) {
  // Compatibility shim: route unqualified legacy paths to the "game:" virtual root.
  // Fully-qualified schemes ("game:/...", "engine:/...") pass through directly.
  std::string mounted = virtualPath;
  if (!virtualPath.empty() && virtualPath.find(':') == std::string::npos)
    mounted = "game:/" + virtualPath;
  auto resolved = Vfs::Get().Resolve(mounted);
  if (resolved)
    return resolved->string();

  // NOTE: Fallback to direct path concatenation if Vfs resolution produces no match.
  return (m_RootPath / "assets" / virtualPath).string();
}

std::filesystem::path ResolveAssetPath(const std::string &path) {
  if (path.empty())
    return {};

  std::error_code ec;
  const fs::path candidate(path);

  // Absolute paths (file dialogs, engine assets) and paths that already resolve
  // against the process working directory are used as they are.
  if (candidate.is_absolute() || fs::exists(candidate, ec))
    return candidate;

  // Project assets first, engine assets second: Mount() keeps "game:" ahead of
  // "engine:" so a project can override an engine-provided file by name.
  for (const char *scheme : {"game:", "engine:"}) {
    if (auto resolved = Vfs::Get().Resolve(std::string(scheme) + path)) {
      if (fs::exists(*resolved, ec))
        return *resolved;
    }
  }

  return candidate;
}

} // namespace UHE
