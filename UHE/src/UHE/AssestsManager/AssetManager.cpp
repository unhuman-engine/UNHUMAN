#include "AssetManager.h"
#include "UHE/Core/Log.h"
#include "UHE/Jobsystem/Jobsystem.h"
#include <chrono>
#include <fstream>

namespace UHE
{

// ---- built-in GPU-free loaders ---------------------------------------------

namespace {

class TextLoader : public IAssetLoader
{
public:
    AssetType Type() const override { return AssetType::Text; }

    Ref<void> LoadFromFile(const std::filesystem::path& path) override
    {
        std::ifstream file(path, std::ios::in);
        if (!file)
            return nullptr;
        auto asset = CreateRef<TextAsset>();
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        asset->Content = std::move(content);
        return asset;
    }
};

class BinaryLoader : public IAssetLoader
{
public:
    AssetType Type() const override { return AssetType::Binary; }

    Ref<void> LoadFromFile(const std::filesystem::path& path) override
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return nullptr;
        auto asset = CreateRef<BinaryAsset>();
        asset->Bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return asset;
    }
};

std::uintmax_t LastWriteTimeOrZero(const std::filesystem::path& path)
{
    std::error_code ec;
    auto time = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : std::uintmax_t(time.time_since_epoch().count());
}

} // namespace

// ---- manager ---------------------------------------------------------------

AssetManager::AssetManager()
{
    // Built-in GPU-free loaders are always available; heavier loaders
    // (textures, models, audio) register themselves from engine code.
    RegisterLoader(std::make_shared<TextLoader>());
    RegisterLoader(std::make_shared<BinaryLoader>());
}

AssetManager& AssetManager::Get()
{
    static AssetManager instance;
    return instance;
}

void AssetManager::RegisterLoader(std::shared_ptr<IAssetLoader> loader)
{
    if (!loader)
        return;
    std::scoped_lock lock(m_Mutex);
    m_Loaders[loader->Type()] = std::move(loader);
}

std::shared_ptr<AssetRecord> AssetManager::CreateRecord(const std::string& path, AssetType type,
                                                        const std::shared_ptr<IAssetLoader>& loader)
{
    auto record = std::make_shared<AssetRecord>();
    record->id = UUID();
    record->sourcePath = path;
    record->type = type;
    record->loader = loader;
    record->fileTime = LastWriteTimeOrZero(path);
    m_Registry[record->id] = record;
    m_PathToID[path] = record->id;
    return record;
}

template <class T>
AssetHandle<T> AssetManager::Load(std::string_view path)
{
    constexpr AssetType type = AssetTraits<T>::Type;
    std::shared_ptr<AssetRecord> record;
    {
        std::scoped_lock lock(m_Mutex);
        auto existing = m_PathToID.find(std::string(path));
        if (existing != m_PathToID.end())
        {
            // Path -> ID dedup: a second request shares the first decode.
            record = m_Registry[existing->second];
        }
        else
        {
            auto loaderIt = m_Loaders.find(type);
            if (loaderIt == m_Loaders.end())
            {
                UHE_CORE_WARN("AssetManager: no loader registered for asset type {0} ('{1}')", int(type),
                              std::string(path));
                return AssetHandle<T>();
            }
            record = CreateRecord(std::string(path), type, loaderIt->second);
        }
    }

    // Fast path: already finished (this or an earlier Load).
    if (record->state.load(std::memory_order_acquire) == LoadState::Ready && record->finalized)
        return AssetHandle<T>(record);

    // Another async load is still decoding this record; its Update()
    // delivery will finish it. Report the shared handle in its current state.
    if (record->state.load(std::memory_order_acquire) == LoadState::Loading)
        return AssetHandle<T>(record);

    // Synchronous decode on this thread.
    record->state.store(LoadState::Loading, std::memory_order_release);
    record->data = record->loader ? record->loader->LoadFromFile(record->sourcePath) : nullptr;
    if (record->data)
    {
        record->state.store(LoadState::Ready, std::memory_order_release);
        FinalizeRecord(*record);
    }
    else
    {
        record->state.store(LoadState::Failed, std::memory_order_release);
        UHE_CORE_WARN("AssetManager: failed to load '{0}'", record->sourcePath);
    }
    return AssetHandle<T>(record);
}

template <class T>
AssetHandle<T> AssetManager::LoadAsync(std::string_view path, LoadCallback onDone)
{
    constexpr AssetType type = AssetTraits<T>::Type;
    std::shared_ptr<AssetRecord> record;
    {
        std::scoped_lock lock(m_Mutex);
        auto existing = m_PathToID.find(std::string(path));
        if (existing != m_PathToID.end())
            record = m_Registry[existing->second];
        else
        {
            auto loaderIt = m_Loaders.find(type);
            if (loaderIt == m_Loaders.end())
            {
                UHE_CORE_WARN("AssetManager: no loader registered for asset type {0} ('{1}')", int(type),
                              std::string(path));
                if (onDone)
                    m_PendingCallbacks.emplace_back(AssetID{}, std::move(onDone));
                return AssetHandle<T>();
            }
            record = CreateRecord(std::string(path), type, loaderIt->second);
        }
    }

    {
        std::scoped_lock lock(m_Mutex);
        m_PendingCallbacks.emplace_back(record->id, std::move(onDone));
    }

    // Already finished - deliver on the next Update().
    if (record->state.load(std::memory_order_acquire) == LoadState::Ready && record->finalized)
        return AssetHandle<T>(record);

    if (!m_Jobs)
    {
        // No jobsystem wired: fall back to a synchronous decode so the caller
        // still gets a valid handle.
        record->data = record->loader ? record->loader->LoadFromFile(record->sourcePath) : nullptr;
        if (record->data)
        {
            record->state.store(LoadState::Ready, std::memory_order_release);
            FinalizeRecord(*record);
        }
        else
        {
            record->state.store(LoadState::Failed, std::memory_order_release);
        }
        return AssetHandle<T>(record);
    }

    record->state.store(LoadState::Loading, std::memory_order_release);
    // Worker: pure CPU work, no RHI. State flips to Ready with release so the
    // main thread's acquire load sees the decoded data.
    std::weak_ptr<AssetRecord> weak = record;
    std::string pathCopy = record->sourcePath;
    m_Jobs->Execute([weak, pathCopy]()
    {
        auto locked = weak.lock();
        if (!locked)
            return;
        Ref<void> data = locked->loader ? locked->loader->LoadFromFile(pathCopy) : nullptr;
        locked->data = data;
        locked->state.store(data ? LoadState::Ready : LoadState::Failed, std::memory_order_release);
    });

    return AssetHandle<T>(record);
}

template <class T>
AssetHandle<T> AssetManager::Get(AssetID id)
{
    std::scoped_lock lock(m_Mutex);
    auto it = m_Registry.find(id);
    if (it == m_Registry.end() || it->second->type != AssetTraits<T>::Type)
        return AssetHandle<T>();
    return AssetHandle<T>(it->second);
}

void AssetManager::FinalizeRecord(AssetRecord& record)
{
    // Main thread only. Finalize does GPU/upload work; callbacks fire after.
    if (record.loader)
        record.loader->Finalize(record.data);
    record.finalized = true;
}

void AssetManager::Unload(AssetID id)
{
    std::scoped_lock lock(m_Mutex);
    auto it = m_Registry.find(id);
    if (it == m_Registry.end())
        return;
    it->second->wantsUnload.store(true, std::memory_order_release);
}

void AssetManager::UnloadUnused()
{
    std::scoped_lock lock(m_Mutex);
    for (auto it = m_Registry.begin(); it != m_Registry.end();)
    {
        const auto& record = it->second;
        if (record->refCount.load(std::memory_order_relaxed) == 0)
        {
            m_PathToID.erase(record->sourcePath);
            it = m_Registry.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void AssetManager::Update()
{
    std::vector<std::pair<AssetID, LoadCallback>> deliver;
    {
        std::scoped_lock lock(m_Mutex);

        // Finalize finished async loads on the main thread.
        for (auto& [id, record] : m_Registry)
        {
            if (record->finalized)
                continue;
            LoadState state = record->state.load(std::memory_order_acquire);
            if (state == LoadState::Ready)
                FinalizeRecord(*record);
            else if (state == LoadState::Failed)
                record->finalized = true; // terminal; nothing to finalize
        }

        // Hot reload polling: mtime-driven, low frequency in practice.
        if (m_HotReload)
        {
            for (auto& [id, record] : m_Registry)
            {
                if (record->state.load(std::memory_order_acquire) == LoadState::Ready && record->finalized)
                    ReloadIfChanged(*record);
            }
        }

        // Reclaim records flagged by Unload() whose handles are all gone.
        for (auto it = m_Registry.begin(); it != m_Registry.end();)
        {
            const auto& record = it->second;
            if (record->wantsUnload.load(std::memory_order_acquire) &&
                record->refCount.load(std::memory_order_relaxed) == 0)
            {
                m_PathToID.erase(record->sourcePath);
                it = m_Registry.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Deliver only callbacks whose load reached a terminal state (or whose
        // record was reclaimed); the rest wait for a later Update().
        for (auto it = m_PendingCallbacks.begin(); it != m_PendingCallbacks.end();)
        {
            auto recordIt = m_Registry.find(it->first);
            bool terminal = recordIt == m_Registry.end() || recordIt->second->finalized;
            if (terminal)
            {
                deliver.emplace_back(std::move(*it));
                it = m_PendingCallbacks.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Deliver outside the lock so callbacks may call back into the manager.
    for (auto& [id, callback] : deliver)
    {
        if (!callback)
            continue;
        auto it = m_Registry.find(id);
        bool success = it != m_Registry.end() && it->second->state.load() == LoadState::Ready &&
                       it->second->finalized;
        callback(id, success);
    }
}

void AssetManager::ReloadIfChanged(AssetRecord& record)
{
    std::uintmax_t current = LastWriteTimeOrZero(record.sourcePath);
    if (current == 0 || current == record.fileTime)
        return;

    record.fileTime = current;
    Ref<void> fresh = record.loader ? record.loader->LoadFromFile(record.sourcePath) : nullptr;
    if (!fresh)
    {
        UHE_CORE_WARN("AssetManager: hot reload failed for '{0}'", record.sourcePath);
        return;
    }
    record.data = std::move(fresh);
    record.finalized = false;
    FinalizeRecord(record);
    record.generation.fetch_add(1, std::memory_order_release);
    // Old handles keep pointing at the same record but their generation check
    // now fails, so they re-fetch or react through their listeners.
    for (auto& listener : record.listeners)
        listener();
    UHE_CORE_INFO("AssetManager: hot reloaded '{0}'", record.sourcePath);
}

void AssetManager::AddListener(AssetID id, std::function<void()> listener)
{
    std::scoped_lock lock(m_Mutex);
    auto it = m_Registry.find(id);
    if (it != m_Registry.end())
        it->second->listeners.push_back(std::move(listener));
}

LoadState AssetManager::GetState(AssetID id) const
{
    std::scoped_lock lock(m_Mutex);
    auto it = m_Registry.find(id);
    return it != m_Registry.end() ? it->second->state.load() : LoadState::Queued;
}

bool AssetManager::IsReady(AssetID id) const
{
    std::scoped_lock lock(m_Mutex);
    auto it = m_Registry.find(id);
    return it != m_Registry.end() && it->second->state.load() == LoadState::Ready &&
           it->second->finalized;
}

AssetID AssetManager::FindByPath(std::string_view path) const
{
    std::scoped_lock lock(m_Mutex);
    auto it = m_PathToID.find(std::string(path));
    return it != m_PathToID.end() ? it->second : AssetID{};
}

size_t AssetManager::RegisteredCount() const
{
    std::scoped_lock lock(m_Mutex);
    return m_Registry.size();
}

// The manager is templated over payload types; instantiate the supported set
// here so the TU owns the code and clients only need the header.
template AssetHandle<TextAsset> AssetManager::Load<TextAsset>(std::string_view);
template AssetHandle<BinaryAsset> AssetManager::Load<BinaryAsset>(std::string_view);
template AssetHandle<TextAsset> AssetManager::LoadAsync<TextAsset>(std::string_view, LoadCallback);
template AssetHandle<BinaryAsset> AssetManager::LoadAsync<BinaryAsset>(std::string_view, LoadCallback);
template AssetHandle<TextAsset> AssetManager::Get<TextAsset>(AssetID);
template AssetHandle<BinaryAsset> AssetManager::Get<BinaryAsset>(AssetID);

} // namespace UHE
