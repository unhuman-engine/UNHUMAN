#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace fs = std::filesystem;

namespace UHE
{

/**
 * @brief Mount-based Virtual File System (VFS).
 *
 * Resolves virtual schemes (e.g. "game:/textures/wall.png", "engine:/shaders/pbr.slang")
 * to physical filesystem paths. Supports layered mounts for modding and asset overrides.
 */
class UHE_API Vfs
{
public:
    static Vfs& Get()
    {
        static Vfs instance;
        return instance;
    }

    /**
     * @brief Mounts a virtual prefix to a physical directory.
     * @param virtualRoot Virtual prefix (e.g. "game:", "engine:").
     * @param physicalRoot Physical host path on disk.
     * Re-mounting an existing prefix replaces its root while preserving mount order.
     */
    void Mount(std::string_view virtualRoot, const fs::path& physicalRoot);

    /**
     * @brief Resolves a virtual path using the highest-priority matching mount.
     * @return Absolute normalized path, or std::nullopt if no mount matched.
     */
    std::optional<fs::path> Resolve(std::string_view virtualPath) const;

    /**
     * @brief Returns all matching paths across layered mounts in priority order.
     */
    std::vector<fs::path> ResolveAll(std::string_view virtualPath) const;

    const std::vector<std::pair<std::string, fs::path>>& GetMounts() const { return m_Mounts; }

private:
    Vfs() = default;

    std::vector<std::pair<std::string, fs::path>> m_Mounts;
};

/**
 * @brief Legacy file system accessor maintained for backward compatibility.
 *
 * Automatically initializes default "game:" and "engine:" mounts.
 * Forwarding shims redirect legacy unqualified paths to Vfs.
 * @deprecated Use Vfs::Get().Resolve(...) for new code.
 */
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

/**
 * @brief Resolves an asset path handed in by content (scene files, inspector
 * fields, drag & drop) against the mounted roots.
 *
 * A path that starts with "game:" or "engine:" resolves against that mount
 * only ("engine:/icon/x.png", the leading slash optional). Otherwise absolute
 * paths and paths that already exist relative to the process are returned
 * unchanged, then the project mount ("game:") is tried first and the engine
 * asset folder ("engine:") second. When nothing matches the original path is
 * returned so the caller reports the failure it always did. The exporter
 * (GameExporter::CollectAssets) follows the same rules.
 */
UHE_API std::filesystem::path ResolveAssetPath(const std::string& path);
} // namespace UHE
