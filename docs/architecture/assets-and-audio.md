<!-- UHE docs — index: ../README.md · roadmap: ../ROADMAP.md -->

> **Status:** `DESIGN`. Phase A below is the intended first step and is not
> started. The immediate audio fixes are independent of the design and can land
> at any time. Related issue:
> [#24](https://github.com/unhuman-engine/UNHUMAN/issues/24) for visualization. The
> audio backend itself landed earlier with miniaudio.
> **Code lands in:** `UHE/src/UHE/AssestsManager/`, `UHE/src/UHE/Audio/`,
> `UHE/src/UHE/Renderer3D/`.
> **Snapshot:** written against the current tree; re-verify `file:line`
> references after phases A to C land.

# Asset system and audio

Companion to [jobsystem.md](jobsystem.md) for async execution and
[framegraph-and-rendergraph.md](framegraph-and-rendergraph.md) for the render
side.

## Summary

The decision that is expensive to change later is asset identity, meaning stable
IDs rather than paths. That should be done first; the rest can wait. The plan is
a small asset spine: an `AssetID`, an `AssetHandle<T>`, an `AssetManager` with a
registry, refcounting and dependencies, and async loading on the job system.

The largest audio defect is that every play reopens and decodes the file
(`MiniAudioBackend.cpp:78-79`). The asset spine fixes it, with a `SoundAsset`
cache, voice handles and an audio bus mixer as the target. The cook and packaging
pipeline and asset streaming are real but deferred. Both systems are the first
real clients of the job system, so building them also exercises it.

## Current state

### Assets

| Piece | Location | State |
|---|---|---|
| Path resolver | `AssestsManager/VfsSystem.h/.cpp` | Singleton, linear search for `UHE_EDITOR/assets`, `Resolve` prepends the root |
| Shader manager | `AssestsManager/ShaderManager.h` | Stub, `void Init()` |
| Model loading | `Renderer3D/LoadModel.h/.cpp` | Synchronous `Model::loadModel(path)` using fastgltf |
| Textures | `Renderer/Texture.h` | `Texture2D` by path (stb_image) and from memory; KTX2/Basis decoded via `Renderer/KTX2.h` (LDR, level 0 + file-authored chains, issue #42); sampler state (wrap/filter/colour space/anisotropy/maxLod) per `RHI::SamplerDesc`; per-load texture dedup in the glTF loader |
| UUID | `Core/UIID.h/.cpp` | Exists, random `u64`; the uniqueness set is unused |
| Ref types | `Core/Core.h:59,66` | `Scope` is `unique_ptr`, `Ref` is `shared_ptr` |

Missing: an `AssetManager`, `AssetHandle`, registry, importer interface,
refcounting by ID, async loading, a dependency graph, hot reload, stable IDs in
serialized data, and a cook step.

Two concrete problems:

1. Two asset roots. `FileSystem::Resolve` always prepends `m_RootPath/assets`
   (`VfsSystem.cpp:35-37`) and `Initialize` looks specifically for
   `UHE_EDITOR/assets` (`:14`). The game target bypasses `Resolve` and builds
   `GetRootPath().parent_path() / "UHEGAME/assets/..."` by hand. The two schemes
   will drift.
2. Path-based references. `Model3DComponent.ModelPath` and texture paths mean
   renaming or moving an asset silently breaks serialized scenes and prefabs.

### Audio

| Piece | Location | State |
|---|---|---|
| Public facade | `Audio/AudioEngine.h/.cpp` | Static, stateless pass-through |
| Backend | `Audio/MiniAudioBackend.cpp` | miniaudio, global engine and a global `std::vector<ma_sound*>` |

Defects:

- Decode per play. `PlaySound2D` and `PlaySound3D` do `new ma_sound` and
  `ma_sound_init_from_file` on every call (`:78-79`, `:98-101`), so every shot
  incurs disk I/O and decoding.
- No control surface. Sounds are fire-and-forget, with no handle to stop, pause,
  or change volume or pitch after starting.
- Not thread-safe. `s_ActiveSounds` is a plain global vector (`:18`) mutated from
  both `Update` and the play functions.
- No mixing. There are no master, SFX or music buses, no ducking and no
  categories.
- The listener is manual. The caller must push the camera position and
  orientation every frame.
- Format limits. `MA_NO_MP3` and `MA_NO_FLAC` (`:6-7`) restrict playback to the
  WAV family.
- Cleanup depends on `Update` being called, so a finished but unswept sound leaks
  until shutdown.

## Asset system

### Identity

```cpp
using AssetID = UHE::UUID;   // reuse Core/UIID.h
```

`UIID.cpp` should be fixed first: the `s_UUIDMap` uniqueness set on line 12 is
never consulted, so two random IDs could collide. Either remove the set or use it
to re-roll on collision. Renaming `UIID` to `UUID` is also worth doing while the
codebase is small.

The stable ID is the contract. Serialized scenes and components store the ID, and
the registry maps ID to the current source path. A rename updates the path, not
the ID, so nothing downstream breaks.

### Handles and refcounting

```cpp
template <class T>
class AssetHandle {
    AssetID m_ID;
    Ref<T>  m_Ptr;
public:
    AssetID ID() const;
    T* Get() const;
    bool IsValid() const;
};
```

`Ref<T>` already exists, and handles can rely on it for lifetime. The simplest
correct start is for handles to hold `Ref<T>` and the manager to hold one as
well, so unload waits for the last handle. The internal slot should carry a
generation so a stale handle to a reloaded asset can be detected rather than
aliasing a new asset.

### Manager and registry

```cpp
class AssetManager {
public:
    static AssetManager& Get();

    template <class T> AssetHandle<T> Load(std::string_view path);
    template <class T> AssetHandle<T> LoadAsync(std::string_view path);
    template <class T> AssetHandle<T> Get(AssetID id);

    void Unload(AssetID id);
    void UnloadUnused();
    void Update();

private:
    std::unordered_map<AssetID, AssetRecord> m_Registry;
    std::unordered_map<std::string, AssetID> m_PathToID;
    std::mutex m_Mutex;
};

struct AssetRecord {
    AssetID id;
    std::string sourcePath;
    AssetType type;
    LoadState state;          // Queued, Loading, Ready, Failed
    u32 refCount;
    std::vector<AssetID> dependencies;
};
```

Load by ID and load by path share one path-to-ID map, so a second request while
the first is in flight returns the same handle instead of decoding twice. Each
record has a state machine of queued, loading, ready or failed, and async
completion is delivered on the main thread in `Update`, so callbacks touch engine
state safely. Unload is refcount-driven rather than immediate from destroy call
sites.

### Importer interface

```cpp
class IAssetLoader {
public:
    virtual AssetType Type() const = 0;
    virtual Ref<void> LoadFromFile(const std::filesystem::path&) = 0;  // CPU, worker-safe
    virtual void Finalize(Ref<void>&) = 0;                            // main-thread GPU upload
};
```

`LoadFromFile` is pure CPU work: parsing glTF, decoding images or decoding audio.
`Finalize` runs on the main thread and performs GPU work, such as creating
buffers and textures or uploading through the render graph once it exists.
Loaders cover models, textures, audio, shaders and scenes; the shader loader
fills the current stub.

### Async loading

```
LoadAsync(path):
    if cached, return handle
    record.state = Queued
    jobsystem.Execute([record]{
        asset = loader.LoadFromFile(record.path);   // worker
        record.state = asset ? Ready : Failed;
    });
    return handle

Update():                                            // main thread, once per frame
    for each record: if Ready and not finalized:
        loader.Finalize(asset)
```

`ParallelFor` handles batch loads such as preloading a level. `TaskGraph` handles
loads with dependencies, for example a model that references textures. Loaders
must not touch the RHI; that is what `Finalize` is for.

### Dependencies

An asset can reference others, for example a model referencing materials and
textures. The record stores its dependencies so that they stay alive while the
parent is alive, and so that finalization order is driven by a `TaskGraph` edge
with dependencies finalizing first. This also provides the data a future pack or
cook step needs, without building that step now.

### Hot reload

A file watcher, which can be a low-frequency poll on a worker, maps a changed
path to an asset ID, reloads, and sets the state to ready with a new generation.
Components holding old handles either re-fetch or are notified through a listener
list per asset. Hot reload is the highest-value feature for an R&D engine, but it
is optional and additive and should not block the spine.

### Unified VFS

Replace the two roots with explicit mounts:

```cpp
class Vfs {
    void Mount(std::string_view virtualRoot, fs::path physicalRoot);
    std::optional<fs::path> Resolve(std::string_view virtualPath);
    std::vector<fs::path>  ResolveAll(std::string_view);
};
```

`game:/models/gun.glb` resolves per project and `engine:/shaders/...` per engine.
`ResolveAll` supports override and mod layering later. Both the editor and game
targets use the same mounts instead of building paths by hand. The old
`FileSystem::Resolve` can remain a shim forwarding to the VFS during migration.

### Cook step

Eventually, source assets are imported into a binary runtime format with flat
buffers, pre-decoded textures and compressed audio, described by a manifest keyed
by asset ID, so the runtime never parses glTF or PNG on the hot path. This is not
needed yet, but the dependency list and stable IDs from the sections above are
exactly what the cook step will use.

### Migrating paths to IDs

1. Add `AssetID` to the registry and seed one ID per existing path on first load,
   persisted in a manifest so IDs are stable across runs.
2. Add `AssetID` alongside the existing model and texture paths, with loaders
   accepting either.
3. Prefer the ID on load and keep the path fallback for old scenes.
4. Migrate serialized scenes to write IDs and drop paths once migrated.
5. Repoint the editor and game path construction at the VFS.

Doing the first two steps early is the point; after that the migration is
additive.

## Audio

### Immediate fixes

1. Cache decoded sounds. At minimum keep a map from path to `ma_audio_buffer` and
   create voices from the cached decode instead of calling
   `ma_sound_init_from_file` per play.
2. Guard `s_ActiveSounds` with a mutex, or make the API main-thread-only and
   assert it.
3. Return a voice handle so callers can stop, pause and set volume.
4. Add master volume.
5. Sync the listener from the active camera each frame.
6. Re-enable MP3 and FLAC once the GCC `-O0` workaround is revisited, or
   standardize on a supported format.

### Sound assets, voices and buses

```cpp
class SoundAsset {                 // loaded via AssetManager
    bool streaming;                // long music streams, short SFX are in memory
};

struct VoiceHandle { u32 index; u32 generation; };

class AudioEngine {
    VoiceHandle Play(const AssetHandle<SoundAsset>&, const PlayParams&);
    void Stop(VoiceHandle); void Pause(VoiceHandle); void SetVolume(VoiceHandle, f32);
    void SetBusVolume(Bus, f32);
    void SetListener(const glm::vec3& pos, const glm::vec3& fwd, const glm::vec3& up);
    void Update();
};

struct PlayParams {
    f32 volume = 1.0f; bool loop = false; bool spatial = true;
    f32 pitch = 1.0f; f32 minDist = 1.0f; f32 maxDist = 50.0f;
    Bus bus = Bus::SFX;
};
```

One decoded `SoundAsset` feeds many concurrent voices, which is the performance
fix. Voices use a pool with generation indices, matching the asset handle scheme.
Buses, for example master to music, SFX, voice and ambient, provide group volume
and ducking; miniaudio supports this through `ma_sound_group`.

### ECS integration

`AudioSourceComponent` holds a clip handle, a bus, loop and play-on-awake flags,
volume and distance range. `AudioListenerComponent` sits on the camera entity,
and `AudioEngine::Update` reads the active listener transform and drives
`SetListener`, which removes the manual call. An `AudioSystem` reacts to
component changes and to start and stop events.

### Events and variation

Gameplay should not think in files. An event such as `event:/Weapon/Shot`
resolves to one or more sound assets and can include random variation, a pitch
range and per-surface mappings. This layer is what makes audio feel like a game
rather than a jukebox, and it is cheap once assets and voices exist.

### Threading

Decoding happens on job-system workers through the asset loader. Voice start and
stop and the mix run on the main thread, while the miniaudio callback mixes on
its own thread. The API should be bounded to the main thread and documented as
such, or use a lock-free command queue into `Update`.

## Integration

The job system gets its first real client in async asset loading, using
`TaskGraph` for dependency-ordered loads and `ParallelFor` for batch preload.
`Finalize` is where textures and buffers become GPU resources, and once the
render graph exists, uploads can be graph passes rather than immediate submits.
Scene serialization should store asset IDs rather than paths, which is the change
that makes the asset work pay off immediately. The editor asset browser reads the
same manager.

## Phases

Phase A, identity and registry: fix UUID uniqueness, add the `AssetID` alias, add
an `AssetManager` skeleton with registry, path-to-ID, refcount and synchronous
`Load`, and route model, texture and audio loading through it. The exit criterion
is that loading the same asset twice returns one instance and that counts drop to
zero on unload.

Phase B, async and audio quick wins: add `IAssetLoader` with worker load and
main-thread finalize, `LoadAsync` on the job system and delivery in
`AssetManager::Update`, plus the audio decoded-sound cache, voice handles, master
volume and automatic listener. The exit criterion is that a level preloads
off-thread without stalling frames and that rapid shots do not hitch or spike
I/O.

Phase C, VFS and IDs in scenes: mount-based VFS with unified editor and game
roots, and scenes and components serializing `AssetID` with a path fallback. The
exit criterion is that renaming an asset file does not break a saved scene.

Phase D, dependencies, buses and events: dependency graph with `TaskGraph`
ordering, audio buses and ducking, and event assets with variation.

Phase E, optional polish: hot reload for shaders, textures and audio, streaming
for music, and pooled readback and upload. The cook pipeline, mod override
layering and platform variants remain deferred.

## Open questions

- Handle lifetime policy: strong `Ref` in handles, which is safe but keeps assets
  resident while referenced, versus weak plus explicit pinning. Start strong.
- ID generation: random at import stored in a manifest, or a content hash of the
  source file. Random is simpler; a content hash dedupes and detects changes but
  changes on every edit.
- Who owns decode: the asset loader, or a dedicated audio loader with its own
  cache. Prefer the loader so audio benefits from the same registry.
- Threading contract: a main-thread-only audio API, which is simpler, or a
  command queue, which is more flexible.
- Hot reload scope: all asset types, or shaders and textures first.
- Renaming `AssestsManager` and `UIID` while churn is low.
