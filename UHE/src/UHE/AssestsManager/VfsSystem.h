#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace fs = std::filesystem;

namespace UHE
{

// Issue #38: mount-based virtual file system. Virtual paths look like
// "game:/models/gun.glb" or "engine:/shaders/Basic3D.slang" and resolve per
// mount, in order, so override and mod layering fall out of ResolveAll().
//
// Both the editor and game targets use the same mounts instead of building
// paths by hand. The legacy FileSystem singleton stays as a shim forwarding
// to the default mount during migration.
class UHE_API Vfs
{
public:
    static Vfs& Get()
    {
        static Vfs instance;
        return instance;
    }

    // Mounts virtualRoot (e.g. "game:") at a physical directory. Remounting
    // an existing virtual root replaces it, keeping mount order stable.
    void Mount(std::string_view virtualRoot, const fs::path& physicalRoot);

    // First mount whose root matches; nullopt when no mount matches.
    std::optional<fs::path> Resolve(std::string_view virtualPath) const;

    // Every mount that matches, in order - later mounts are override layers.
    std::vector<fs::path> ResolveAll(std::string_view virtualPath) const;

    const std::vector<std::pair<std::string, fs::path>>& GetMounts() const { return m_Mounts; }

private:
    Vfs() = default;

    std::vector<std::pair<std::string, fs::path>> m_Mounts;
};

// Legacy singleton kept during the path -> ID / VFS migration. Initialize
// discovers the project root and mounts it as "game:" (and the engine asset
// directory as "engine:" when found); Resolve forwards into the VFS.
class UHE_API FileSystem
{
public:
    static FileSystem& Get()
    {
        static FileSystem instance;
        return instance;
    }

    void Initialize(const char* argv0);

    // Legacy entry point: resolves relative to the default "game:" mount.
    std::string Resolve(const std::string& virtualPath);

    const fs::path& GetRootPath() const { return m_RootPath; }

private:
    FileSystem() = default;
    std::filesystem::path m_RootPath;
};
} // namespace UHE
