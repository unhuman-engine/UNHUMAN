#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "UHE/AssestsManager/AssetID.h"
#include "UHE/Core/Core.h"

namespace UHE::Jobsystem
{
class UheJobsystem;
}

namespace UHE
{

// ---- asset types -----------------------------------------------------------

enum class AssetType : u32
{
    None,
    Text,
    Binary,
    Texture,
    Model,
    Audio,
    Shader,
    Scene
};

// Built-in, GPU-free asset payloads. Texture/model/audio payloads arrive once
// their loaders exist; the manager is agnostic - it only moves Ref<void>.
struct UHE_API TextAsset
{
    std::string Content;
};

struct UHE_API BinaryAsset
{
    std::vector<u8> Bytes;
};

// Maps a C++ payload type to its registry type tag.
template <class T> struct AssetTraits;
template <> struct AssetTraits<TextAsset>
{
    static constexpr AssetType Type = AssetType::Text;
};
template <> struct AssetTraits<BinaryAsset>
{
    static constexpr AssetType Type = AssetType::Binary;
};

// ---- load pipeline ---------------------------------------------------------

enum class LoadState : u32
{
    Queued,   // registered, waiting for a worker
    Loading,  // LoadFromFile running on a worker
    Ready,    // CPU data present (finalize may still be pending)
    Failed    // LoadFromFile failed
};

// Issue #38: importer interface. LoadFromFile is pure CPU work and must be
// safe to call on a worker thread; Finalize runs on the main thread and does
// the GPU/upload work. Loaders must never touch the RHI from LoadFromFile.
class IAssetLoader
{
public:
    virtual ~IAssetLoader() = default;
    virtual AssetType Type() const = 0;
    virtual Ref<void> LoadFromFile(const std::filesystem::path& path) = 0;
    virtual void Finalize(Ref<void>&) {}
};

struct AssetRecord
{
    AssetID id;
    std::string sourcePath;
    AssetType type = AssetType::None;
    std::atomic<LoadState> state{LoadState::Queued};
    // Live handles referencing this record. The manager itself holds no ref;
    // records with refCount == 0 are reclaimable cache entries.
    std::atomic<u32> refCount{0};
    // Bumped on every reload so stale handles can be detected.
    std::atomic<u32> generation{1};
    // Set by Unload; the record is erased by Update/UnloadUnused once
    // refCount drains to zero.
    std::atomic<bool> wantsUnload{false};
    // Main-thread-only flag: Finalize has run and callbacks have fired.
    bool finalized = false;

    Ref<void> data;                       // valid when state == Ready
    std::shared_ptr<IAssetLoader> loader; // owns the finalize step
    std::vector<AssetID> dependencies;    // for TaskGraph-ordered finalize later

    // mtime captured at load; polled when hot reload is enabled.
    std::uintmax_t fileTime = 0;

    // Reload/listener notification (main thread).
    std::vector<std::function<void()>> listeners;
};

using LoadCallback = std::function<void(AssetID id, bool success)>;

// ---- handle ----------------------------------------------------------------

// Typed, refcounted view of a loaded asset. Copying a handle bumps the
// record's refcount; destroying it releases. Handles survive the asset being
// unloaded (IsValid simply turns false) and detect reloads through the
// record's generation.
template <class T>
class AssetHandle
{
public:
    AssetHandle() = default;

    T* Get() const
    {
        if (!IsReady())
            return nullptr;
        return static_cast<T*>(m_Record->data.get());
    }

    Ref<T> GetRef() const
    {
        if (!IsReady())
            return nullptr;
        return std::static_pointer_cast<T>(m_Record->data);
    }

    AssetID ID() const { return m_ID; }
    u32 Generation() const { return m_Generation; }

    // True when the asset this handle was created for is loaded AND still the
    // current generation (a reload invalidates older handles).
    bool IsValid() const
    {
        return m_Record && m_Record->finalized && m_Record->generation.load() == m_Generation;
    }
    explicit operator bool() const { return IsValid(); }

private:
    friend class AssetManager;
    explicit AssetHandle(std::shared_ptr<AssetRecord> record)
        : m_Record(std::move(record)),
          m_ID(m_Record->id),
          m_Generation(m_Record->generation.load())
    {
        m_Record->refCount.fetch_add(1, std::memory_order_relaxed);
    }

    // True when CPU data exists, regardless of generation (reload check).
    bool IsReady() const
    {
        return m_Record && m_Record->generation.load(std::memory_order_acquire) == m_Generation &&
               m_Record->state.load(std::memory_order_acquire) == LoadState::Ready &&
               m_Record->finalized && m_Record->data != nullptr;
    }

    void AddRef()
    {
        if (m_Record)
            m_Record->refCount.fetch_add(1, std::memory_order_relaxed);
    }
    void ReleaseRef()
    {
        if (m_Record)
            m_Record->refCount.fetch_sub(1, std::memory_order_relaxed);
    }

    std::shared_ptr<AssetRecord> m_Record;
    AssetID m_ID{};
    u32 m_Generation = 0;

public:
    AssetHandle(const AssetHandle& other)
        : m_Record(other.m_Record), m_ID(other.m_ID), m_Generation(other.m_Generation)
    {
        AddRef();
    }
    AssetHandle(AssetHandle&& other) noexcept
        : m_Record(std::move(other.m_Record)), m_ID(other.m_ID), m_Generation(other.m_Generation)
    {
        other.m_Record = nullptr;
        other.m_Generation = 0;
    }
    AssetHandle& operator=(const AssetHandle& other)
    {
        if (this != &other)
        {
            other.m_Record->refCount.fetch_add(1, std::memory_order_relaxed);
            ReleaseRef();
            m_Record = other.m_Record;
            m_ID = other.m_ID;
            m_Generation = other.m_Generation;
        }
        return *this;
    }
    AssetHandle& operator=(AssetHandle&& other) noexcept
    {
        if (this != &other)
        {
            ReleaseRef();
            m_Record = std::move(other.m_Record);
            m_ID = other.m_ID;
            m_Generation = other.m_Generation;
            other.m_Record = nullptr;
            other.m_Generation = 0;
        }
        return *this;
    }
    ~AssetHandle() { ReleaseRef(); }
};

// ---- manager ---------------------------------------------------------------

// Issue #38: central asset registry. One record per path (path -> ID dedup),
// refcounted typed handles, a Queued/Loading/Ready/Failed state machine, async
// loads kicked onto the jobsystem with main-thread delivery in Update(), and
// optional mtime-polling hot reload.
class UHE_API AssetManager
{
public:
    static AssetManager& Get();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // Optional: enables LoadAsync worker execution. Without it LoadAsync runs
    // synchronously on the calling thread.
    void SetJobsystem(Jobsystem::UheJobsystem* jobs) { m_Jobs = jobs; }
    void RegisterLoader(std::shared_ptr<IAssetLoader> loader);

    // Synchronous load: decodes immediately and returns a ready handle.
    template <class T> AssetHandle<T> Load(std::string_view path);
    // Asynchronous load: queues a worker job; poll the handle or wait for the
    // Update()-delivered callback. Returns a queued (not yet valid) handle.
    template <class T> AssetHandle<T> LoadAsync(std::string_view path, LoadCallback onDone = {});
    // Handle to an already-registered asset (any state); empty if unknown.
    template <class T> AssetHandle<T> Get(AssetID id);

    // Marks an asset for unload; the record is reclaimed by Update() once its
    // last handle is gone.
    void Unload(AssetID id);
    // Immediately reclaims every record with no live handles.
    void UnloadUnused();

    // Main-thread delivery: finalizes finished loads, fires callbacks,
    // performs hot-reload polling and reclaims unloaded records.
    void Update();

    LoadState GetState(AssetID id) const;
    bool IsReady(AssetID id) const;
    AssetID FindByPath(std::string_view path) const;

    // Hot reload: when enabled, Update() polls mtimes of Ready assets and
    // reloads changed files, bumping the generation and notifying listeners.
    void SetHotReloadEnabled(bool enable) { m_HotReload = enable; }
    // Notified (main thread, from Update()) after a reload replaced the data.
    void AddListener(AssetID id, std::function<void()> listener);

    size_t RegisteredCount() const;

private:
    AssetManager();
    ~AssetManager() = default;

    std::shared_ptr<AssetRecord> CreateRecord(const std::string& path, AssetType type,
                                              const std::shared_ptr<IAssetLoader>& loader);
    std::shared_ptr<AssetRecord> FindRecord(AssetID id) const;
    void FinalizeRecord(AssetRecord& record);
    void ReloadIfChanged(AssetRecord& record);

    mutable std::mutex m_Mutex;
    std::unordered_map<AssetID, std::shared_ptr<AssetRecord>> m_Registry;
    std::unordered_map<std::string, AssetID> m_PathToID;
    std::unordered_map<AssetType, std::shared_ptr<IAssetLoader>> m_Loaders;
    std::vector<std::pair<AssetID, LoadCallback>> m_PendingCallbacks;
    Jobsystem::UheJobsystem* m_Jobs = nullptr;
    bool m_HotReload = false;
};

} // namespace UHE
