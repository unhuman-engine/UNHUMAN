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
    return results.front();
}

std::vector<fs::path> Vfs::ResolveAll(std::string_view virtualPath) const
{
    std::vector<fs::path> results;
    for (const auto& [root, physical] : m_Mounts)
    {
        std::string_view prefix = virtualPath;
        // The mount prefix includes the colon ("game:"), so "gameboy:/x"
        // never matches the "game:" mount.
        if (!prefix.starts_with(root))
            continue;
        std::string_view remainder = prefix.substr(root.size());
        if (!remainder.empty() && (remainder.front() == '/' || remainder.front() == '\\'))
            remainder.remove_prefix(1);
        fs::path resolved = physical / fs::path(remainder).lexically_normal();
        results.push_back(resolved.make_preferred());
    }
    return results;
}

// ---- legacy FileSystem shim -------------------------------------------------

void FileSystem::Initialize(const char *argv0) {
  fs::path exePath = fs::absolute(argv0).parent_path();
  bool foundSourceAssets = false;

  // Issue #38: unified roots - the game and editor targets share one
  // discovery path; whichever root exists is mounted as "game:".
  static constexpr const char *kProjectMarkers[] = {"UHE_EDITOR", "UHEGAME"};

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
  // Shim for the migration: prefixless legacy paths go through the default
  // "game:" mount; fully virtual paths ("game:/x", "engine:/x") pass through.
  std::string mounted = virtualPath;
  if (!virtualPath.empty() && virtualPath.find(':') == std::string::npos)
    mounted = "game:/" + virtualPath;
  auto resolved = Vfs::Get().Resolve(mounted);
  if (resolved)
    return resolved->string();

  // Fallback to the pre-VFS behavior so nothing regresses mid-migration.
  return (m_RootPath / "assets" / virtualPath).string();
}

} // namespace UHE
