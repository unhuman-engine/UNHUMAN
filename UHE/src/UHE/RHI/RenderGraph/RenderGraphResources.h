#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "UHE/RHI/RenderGraph/RenderGraphTypes.h"

// Render graph resource registry (docs/architecture/rendergraph.md §8.2, step 1
// slice). Names, versions and generations only — no allocation, no Vulkan
// objects. The alias heap / VMA pool arrives with the executor milestone
// (rendergraph.md §13.2 item 7); this registry is the bookkeeping it plugs into.

namespace UHE::RHI
{

// One declared write bumps the version. The (resource, version) pair is what
// the compiler will turn into edges: reads bind to a version, writes end one
// and start the next (rendergraph.md §7 versioning rules).
struct RGResourceVersion
{
    u32 passIndex = RG::kInvalidIndex; // declaring pass, into RenderGraph::m_Passes
    u32 version = 0;
};

struct RGTextureSlot
{
    std::string name;
    u32 generation = 0; // monotonic across the registry's lifetime — a stale
                        // handle can only fail its generation check, never alias
    bool imported = false;
    RGTextureDesc desc{};    // transient: the allocation contract (compiler consumes)
    RGImportedDesc import{}; // imported: identity of the external resource
    u32 currentVersion = 0;
    std::vector<RGResourceVersion> versionLog; // ordered by version, first entry is v1
};

struct RGBufferSlot
{
    std::string name;
    u32 generation = 0; // monotonic — see RGTextureSlot
    bool imported = false;
    RGBufferDesc desc{};
    RGImportedDesc import{};
    u32 currentVersion = 0;
    std::vector<RGResourceVersion> versionLog;
};

class RenderGraphResources
{
public:
    RenderGraphResources() = default;
    RenderGraphResources(const RenderGraphResources&) = delete;
    RenderGraphResources& operator=(const RenderGraphResources&) = delete;
    ~RenderGraphResources() = default;

    // ─── Creation (declare-time only; never on the hot path) ───

    // Transient resource: the graph owns the future allocation.
    RGTextureHandle CreateTexture(const RGTextureDesc& desc);
    RGBufferHandle CreateBuffer(const RGBufferDesc& desc);

    // Imported resource: allocated outside the graph (swapchain, sensor buffer).
    // identityHash must change when the external resource is replaced (e.g.
    // swapchain generation) — it feeds the topology hash (§9.1.4).
    RGTextureHandle ImportTexture(const std::string& name, const RGImportedDesc& identity, ImageState initialState,
                                  ImageState finalState);
    RGBufferHandle ImportBuffer(const std::string& name, const RGImportedDesc& identity);

    // ─── Registry queries ───

    [[nodiscard]] const RGTextureSlot* GetTexture(RGTextureHandle handle) const;
    [[nodiscard]] const RGBufferSlot* GetBuffer(RGBufferHandle handle) const;
    [[nodiscard]] RGTextureSlot* GetTexture(RGTextureHandle handle);
    [[nodiscard]] RGBufferSlot* GetBuffer(RGBufferHandle handle);

    [[nodiscard]] const std::vector<RGTextureSlot>& Textures() const { return m_Textures; }
    [[nodiscard]] const std::vector<RGBufferSlot>& Buffers() const { return m_Buffers; }

    // "Name.vN" for validation messages (current version); "?" on stale handles.
    [[nodiscard]] std::string TextureName(RGTextureHandle handle) const;
    [[nodiscard]] std::string BufferName(RGBufferHandle handle) const;

    // "Name.vN" for the access dump: names the version an access was BOUND to
    // at declaration time, not the slot's current version (a later write must
    // not retroactively rebind an earlier declaration — §7).
    [[nodiscard]] std::string TextureNameAt(RGTextureHandle handle, u32 version) const;
    [[nodiscard]] std::string BufferNameAt(RGBufferHandle handle, u32 version) const;

    // ─── Versioning (builder-only mutations) ───

    u32 AdvanceTextureVersion(RGTextureHandle handle, u32 passIndex);
    u32 AdvanceBufferVersion(RGBufferHandle handle, u32 passIndex);

    // ─── Frame reset ───
    // Retires every outstanding handle (generations are never handed out twice,
    // so retained handles fail their check even after index reuse) and prunes
    // the slot arrays: a rebuilt frame starts clean, so identical frames produce
    // identical topology hashes (§8.3 cache key) and arrays cannot grow
    // unboundedly. Callers must re-import per frame or per resize — the
    // contract the swapchain path already follows.

    void Reset();

    [[nodiscard]] u64 ComputeTopologyHash() const;

private:
    RGTextureHandle AllocateTextureSlot(std::string name, bool imported);
    RGBufferHandle AllocateBufferSlot(std::string name, bool imported);

    std::vector<RGTextureSlot> m_Textures;
    std::vector<RGBufferSlot> m_Buffers;
    // Next generation for freshly allocated slots (never reset — this is what
    // makes handles retained across Reset() fail deterministically).
    u32 m_NextTextureGeneration = RG::kStartGeneration;
    u32 m_NextBufferGeneration = RG::kStartGeneration;
    // Name → slot index, so a second declaration of "GBufferA" hits the same
    // slot instead of silently creating a second resource.
    std::unordered_map<std::string, u32> m_TextureIndices;
    std::unordered_map<std::string, u32> m_BufferIndices;
};

} // namespace UHE::RHI
