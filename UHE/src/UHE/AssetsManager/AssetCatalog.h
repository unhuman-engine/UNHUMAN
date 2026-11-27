#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "UHE/AssetsManager/AssetID.h"
#include "UHE/Core/Core.h"

namespace UHE
{

/// @brief One asset in an AssetCatalog.
struct AssetCatalogEntry
{
    AssetID ID{0};
    std::string Path;             ///< the path scenes and scripts name, relative to its root ("Models/Tree.gltf")
    std::string Stored;           ///< the file that holds it in a game, when not Path ("Models/Tree.gltf.uhemodel")
    std::string Root = "project"; ///< "project" (the project's Assets/) or "engine" (the engine assets)
    std::string Type;             ///< model, texture, scene, script, audio, font, shader, data
    u64 Size = 0;
    u64 Hash = 0;                      ///< Cooked::HashFile of the stored file
    std::vector<AssetID> Dependencies; ///< what it reads besides itself (a glTF's buffers and images)
};

/**
 * @brief Stable AssetIDs for a project's files, and the manifest a game ships (issue #45).
 *
 * Two files use it:
 * - `<project>/AssetRegistry.yaml` (kRegistryName): every file under the
 *   project's Assets/ with the ID it keeps for good. Commit it: the IDs are
 *   only stable if every machine reads the same registry.
 * - `<game>/Assets/AssetManifest.yaml` (kManifestName): what an export ships,
 *   each asset with its ID, the file that stores it (a cooked model's
 *   .uhemodel), type, size, hash and dependencies. AssetManager::LoadCatalog
 *   reads it, so a game resolves the same ID to the same asset on every run.
 */
class UHE_API AssetCatalog
{
public:
    static constexpr const char* kRegistryName = "AssetRegistry.yaml";
    static constexpr const char* kManifestName = "AssetManifest.yaml";

    /// @brief Reads a registry or manifest; false (with \p error) when it exists and is not one.
    bool Load(const std::filesystem::path& file, std::string* error = nullptr);
    /// @brief Writes it (to a temporary file, then renamed).
    bool Save(const std::filesystem::path& file, std::string* error = nullptr) const;

    /**
     * @brief Gives every (path, hash) a stable ID, and forgets paths no longer given.
     *
     * A path it already knows keeps its ID. A new path whose hash equals that
     * of a known path that is gone takes over that path's ID: the file was
     * moved or renamed, and references by ID still find it. Anything else gets
     * a new random ID. Paths in \p files are generic ("/") and relative.
     *
     * @return true when anything changed (the registry needs saving).
     */
    bool Assign(const std::map<std::string, u64>& files);

    /// @brief An ID that names \p path the same way in every export, for files with no registry (engine assets).
    [[nodiscard]] static AssetID DerivedID(const std::string& root, const std::string& path);

    [[nodiscard]] AssetID IdFor(const std::string& path) const;
    [[nodiscard]] const AssetCatalogEntry* Find(AssetID id) const;
    [[nodiscard]] const AssetCatalogEntry* FindByPath(const std::string& path) const;

    void Add(AssetCatalogEntry entry);
    [[nodiscard]] const std::vector<AssetCatalogEntry>& Entries() const { return m_Entries; }
    [[nodiscard]] std::size_t Size() const { return m_Entries.size(); }

    /// @brief The type an asset's extension names (model, texture, scene, ...).
    [[nodiscard]] static std::string TypeOf(const std::filesystem::path& path);

    std::string Platform; ///< the platform a manifest was exported for (desktop, mobile); empty in a registry

private:
    void Reindex();

    std::vector<AssetCatalogEntry> m_Entries;
    std::map<std::string, std::size_t> m_ByPath;
    std::map<u64, std::size_t> m_ByID;
};

} // namespace UHE
