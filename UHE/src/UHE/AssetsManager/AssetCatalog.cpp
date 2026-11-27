#include "uhepch.h"
#include "UHE/AssetsManager/AssetCatalog.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <yaml-cpp/yaml.h>

namespace UHE
{
namespace fs = std::filesystem;

namespace
{
std::string Hex(u64 value)
{
    char text[19];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

u64 FromHex(const std::string& text)
{
    return text.empty() ? 0 : std::stoull(text, nullptr, 16);
}

void SetError(std::string* error, const std::string& message)
{
    if (error != nullptr)
        *error = message;
}
} // namespace

void AssetCatalog::Reindex()
{
    m_ByPath.clear();
    m_ByID.clear();
    for (std::size_t i = 0; i < m_Entries.size(); ++i)
    {
        m_ByPath[m_Entries[i].Path] = i;
        if (!m_Entries[i].Stored.empty())
            m_ByPath.emplace(m_Entries[i].Stored, i);
        m_ByID[static_cast<u64>(m_Entries[i].ID)] = i;
    }
}

bool AssetCatalog::Load(const fs::path& file, std::string* error)
{
    m_Entries.clear();
    Platform.clear();
    std::error_code ec;
    if (!fs::exists(file, ec))
    {
        Reindex();
        return true; // no registry yet: every file gets a new ID
    }
    try
    {
        const YAML::Node root = YAML::LoadFile(file.string());
        if (root["Platform"])
            Platform = root["Platform"].as<std::string>();
        for (const YAML::Node& node : root["Assets"])
        {
            AssetCatalogEntry entry;
            entry.ID = AssetID(node["ID"].as<u64>());
            entry.Path = node["Path"].as<std::string>();
            if (node["Stored"])
                entry.Stored = node["Stored"].as<std::string>();
            if (node["Root"])
                entry.Root = node["Root"].as<std::string>();
            if (node["Type"])
                entry.Type = node["Type"].as<std::string>();
            if (node["Size"])
                entry.Size = node["Size"].as<u64>();
            if (node["Hash"])
                entry.Hash = FromHex(node["Hash"].as<std::string>());
            if (node["Dependencies"])
                for (const YAML::Node& dependency : node["Dependencies"])
                    entry.Dependencies.emplace_back(dependency.as<u64>());
            m_Entries.push_back(std::move(entry));
        }
    }
    catch (const std::exception& e)
    {
        m_Entries.clear();
        Reindex();
        SetError(error, "'" + file.filename().string() + "' is not an asset catalog: " + e.what());
        return false;
    }
    Reindex();
    return true;
}

bool AssetCatalog::Save(const fs::path& file, std::string* error) const
{
    YAML::Emitter out;
    out << YAML::BeginMap;
    if (!Platform.empty())
        out << YAML::Key << "Platform" << YAML::Value << Platform;
    out << YAML::Key << "Assets" << YAML::Value << YAML::BeginSeq;
    std::vector<const AssetCatalogEntry*> sorted;
    for (const AssetCatalogEntry& entry : m_Entries)
        sorted.push_back(&entry);
    // Sorted by path: a registry in version control then changes by the lines that changed.
    std::ranges::sort(sorted, [](const auto* a, const auto* b)
                      { return std::tie(a->Root, a->Path) < std::tie(b->Root, b->Path); });
    for (const AssetCatalogEntry* entry : sorted)
    {
        out << YAML::BeginMap;
        out << YAML::Key << "ID" << YAML::Value << static_cast<u64>(entry->ID);
        out << YAML::Key << "Path" << YAML::Value << entry->Path;
        if (!entry->Stored.empty())
            out << YAML::Key << "Stored" << YAML::Value << entry->Stored;
        if (entry->Root != "project")
            out << YAML::Key << "Root" << YAML::Value << entry->Root;
        if (!entry->Type.empty())
            out << YAML::Key << "Type" << YAML::Value << entry->Type;
        out << YAML::Key << "Size" << YAML::Value << entry->Size;
        out << YAML::Key << "Hash" << YAML::Value << Hex(entry->Hash);
        if (!entry->Dependencies.empty())
        {
            out << YAML::Key << "Dependencies" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (const AssetID dependency : entry->Dependencies)
                out << static_cast<u64>(dependency);
            out << YAML::EndSeq;
        }
        out << YAML::EndMap;
    }
    out << YAML::EndSeq << YAML::EndMap;

    const fs::path temporary = file.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::trunc);
        stream << "# Stable asset IDs (UHE, issue #45). Generated; commit it with the project.\n"
               << out.c_str() << "\n";
        if (!stream)
        {
            SetError(error, "could not write '" + temporary.string() + "'");
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, file, ec);
    if (ec)
    {
        SetError(error, "could not replace '" + file.string() + "': " + ec.message());
        return false;
    }
    return true;
}

bool AssetCatalog::Assign(const std::map<std::string, u64>& files)
{
    bool changed = false;
    std::vector<AssetCatalogEntry> next;
    std::set<std::size_t> claimed;
    // Known entries whose path is gone, by hash: where a moved file came from.
    std::multimap<u64, std::size_t> vanished;
    for (std::size_t i = 0; i < m_Entries.size(); ++i)
        if (!files.contains(m_Entries[i].Path))
            vanished.emplace(m_Entries[i].Hash, i);

    for (const auto& [path, hash] : files)
    {
        AssetCatalogEntry entry;
        entry.Path = path;
        entry.Hash = hash;
        entry.Type = TypeOf(path);
        if (const auto known = m_ByPath.find(path); known != m_ByPath.end() && m_Entries[known->second].Path == path)
        {
            entry.ID = m_Entries[known->second].ID;
            changed |= m_Entries[known->second].Hash != hash;
            claimed.insert(known->second);
        }
        else
        {
            changed = true;
            const auto [from, to] = vanished.equal_range(hash);
            const auto moved = std::find_if(from, to, [&](const auto& item) { return !claimed.contains(item.second); });
            if (hash != 0 && moved != to)
            {
                entry.ID = m_Entries[moved->second].ID;
                claimed.insert(moved->second);
            }
            else
                entry.ID = AssetID();
        }
        next.push_back(std::move(entry));
    }
    changed |= claimed.size() != m_Entries.size();
    m_Entries = std::move(next);
    Reindex();
    return changed;
}

AssetID AssetCatalog::DerivedID(const std::string& root, const std::string& path)
{
    // FNV-1a of "root:path", never 0 (0 is "no asset").
    u64 hash = 1469598103934665603ull;
    for (const char c : root + ":" + path)
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    return AssetID(hash == 0 ? 1 : hash);
}

AssetID AssetCatalog::IdFor(const std::string& path) const
{
    const AssetCatalogEntry* entry = FindByPath(path);
    return entry != nullptr ? entry->ID : AssetID(0);
}

const AssetCatalogEntry* AssetCatalog::Find(AssetID id) const
{
    const auto it = m_ByID.find(static_cast<u64>(id));
    return it != m_ByID.end() ? &m_Entries[it->second] : nullptr;
}

const AssetCatalogEntry* AssetCatalog::FindByPath(const std::string& path) const
{
    const auto it = m_ByPath.find(fs::path(path).generic_string());
    return it != m_ByPath.end() ? &m_Entries[it->second] : nullptr;
}

void AssetCatalog::Add(AssetCatalogEntry entry)
{
    m_Entries.push_back(std::move(entry));
    Reindex();
}

std::string AssetCatalog::TypeOf(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".uhemodel" || ext == ".gltf" || ext == ".glb")
        return "model";
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".ktx2" || ext == ".hdr" || ext == ".tga" ||
        ext == ".bmp")
        return "texture";
    if (ext == ".unhuman")
        return "scene";
    if (ext == ".lua")
        return "script";
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac")
        return "audio";
    if (ext == ".ttf" || ext == ".otf")
        return "font";
    if (ext == ".spv" || ext == ".slang")
        return "shader";
    return "data";
}

} // namespace UHE
