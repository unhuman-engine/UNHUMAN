#include "uhepch.h"
#include "RenderGraphResources.h"

namespace UHE::RHI
{

// ─── Creation ───────────────────────────────────────────────────────────────

RGTextureHandle RenderGraphResources::AllocateTextureSlot(std::string name, bool imported)
{
    // Name collision returns the existing slot (see m_TextureIndices comment in
    // the header) — the builder turns a genuine re-declaration with a different
    // desc into a validation error, this only makes handles idempotent.
    const auto it = m_TextureIndices.find(name);
    if (it != m_TextureIndices.end())
    {
        RGTextureSlot& slot = m_Textures[it->second];
        // Keep the imported/transient classification consistent when the same
        // name is declared again with the other kind.
        slot.imported = imported;
        return RGTextureHandle{it->second, slot.generation};
    }

    const u32 index = static_cast<u32>(m_Textures.size());
    RGTextureSlot& slot = m_Textures.emplace_back();
    slot.name = std::move(name);
    slot.generation = m_NextTextureGeneration++;
    slot.imported = imported;
    m_TextureIndices.emplace(slot.name, index);
    return RGTextureHandle{index, slot.generation};
}

RGBufferHandle RenderGraphResources::AllocateBufferSlot(std::string name, bool imported)
{
    const auto it = m_BufferIndices.find(name);
    if (it != m_BufferIndices.end())
    {
        RGBufferSlot& slot = m_Buffers[it->second];
        slot.imported = imported;
        return RGBufferHandle{it->second, slot.generation};
    }

    const u32 index = static_cast<u32>(m_Buffers.size());
    RGBufferSlot& slot = m_Buffers.emplace_back();
    slot.name = std::move(name);
    slot.generation = m_NextBufferGeneration++;
    slot.imported = imported;
    m_BufferIndices.emplace(slot.name, index);
    return RGBufferHandle{index, slot.generation};
}

RGTextureHandle RenderGraphResources::CreateTexture(const RGTextureDesc& desc)
{
    RGTextureHandle handle = AllocateTextureSlot(desc.name, /*imported=*/false);
    if (RGTextureSlot* slot = GetTexture(handle))
        slot->desc = desc;
    return handle;
}

RGBufferHandle RenderGraphResources::CreateBuffer(const RGBufferDesc& desc)
{
    RGBufferHandle handle = AllocateBufferSlot(desc.name, /*imported=*/false);
    if (RGBufferSlot* slot = GetBuffer(handle))
        slot->desc = desc;
    return handle;
}

RGTextureHandle RenderGraphResources::ImportTexture(const std::string& name, const RGImportedDesc& identity,
                                                    ImageState initialState, ImageState finalState)
{
    RGTextureHandle handle = AllocateTextureSlot(name, /*imported=*/true);
    if (RGTextureSlot* slot = GetTexture(handle))
    {
        slot->import = identity;
        slot->desc.initialState = initialState;
        slot->desc.finalState = finalState;
    }
    return handle;
}

RGBufferHandle RenderGraphResources::ImportBuffer(const std::string& name, const RGImportedDesc& identity)
{
    RGBufferHandle handle = AllocateBufferSlot(name, /*imported=*/true);
    if (RGBufferSlot* slot = GetBuffer(handle))
        slot->import = identity;
    return handle;
}

// ─── Queries ────────────────────────────────────────────────────────────────

const RGTextureSlot* RenderGraphResources::GetTexture(RGTextureHandle handle) const
{
    if (!handle.IsValid() || handle.index >= m_Textures.size())
        return nullptr;
    const RGTextureSlot& slot = m_Textures[handle.index];
    return slot.generation == handle.generation ? &slot : nullptr;
}

const RGBufferSlot* RenderGraphResources::GetBuffer(RGBufferHandle handle) const
{
    if (!handle.IsValid() || handle.index >= m_Buffers.size())
        return nullptr;
    const RGBufferSlot& slot = m_Buffers[handle.index];
    return slot.generation == handle.generation ? &slot : nullptr;
}

RGTextureSlot* RenderGraphResources::GetTexture(RGTextureHandle handle)
{
    return const_cast<RGTextureSlot*>(std::as_const(*this).GetTexture(handle));
}

RGBufferSlot* RenderGraphResources::GetBuffer(RGBufferHandle handle)
{
    return const_cast<RGBufferSlot*>(std::as_const(*this).GetBuffer(handle));
}

std::string RenderGraphResources::TextureName(RGTextureHandle handle) const
{
    const RGTextureSlot* slot = GetTexture(handle);
    if (!slot)
        return "?";
    return slot->name + ".v" + std::to_string(slot->currentVersion);
}

std::string RenderGraphResources::BufferName(RGBufferHandle handle) const
{
    const RGBufferSlot* slot = GetBuffer(handle);
    if (!slot)
        return "?";
    return slot->name + ".v" + std::to_string(slot->currentVersion);
}

std::string RenderGraphResources::TextureNameAt(RGTextureHandle handle, u32 version) const
{
    const RGTextureSlot* slot = GetTexture(handle);
    if (!slot)
        return "?";
    return slot->name + ".v" + std::to_string(version);
}

std::string RenderGraphResources::BufferNameAt(RGBufferHandle handle, u32 version) const
{
    const RGBufferSlot* slot = GetBuffer(handle);
    if (!slot)
        return "?";
    return slot->name + ".v" + std::to_string(version);
}

// ─── Versioning ─────────────────────────────────────────────────────────────

u32 RenderGraphResources::AdvanceTextureVersion(RGTextureHandle handle, u32 passIndex)
{
    RGTextureSlot* slot = GetTexture(handle);
    UHE_CORE_ASSERT(slot != nullptr, "RenderGraph: AdvanceTextureVersion on stale handle");
    if (!slot)
        return 0;

    ++slot->currentVersion;
    slot->versionLog.push_back(RGResourceVersion{passIndex, slot->currentVersion});
    return slot->currentVersion;
}

u32 RenderGraphResources::AdvanceBufferVersion(RGBufferHandle handle, u32 passIndex)
{
    RGBufferSlot* slot = GetBuffer(handle);
    UHE_CORE_ASSERT(slot != nullptr, "RenderGraph: AdvanceBufferVersion on stale handle");
    if (!slot)
        return 0;

    ++slot->currentVersion;
    slot->versionLog.push_back(RGResourceVersion{passIndex, slot->currentVersion});
    return slot->currentVersion;
}

// ─── Frame reset ────────────────────────────────────────────────────────────

void RenderGraphResources::Reset()
{
    // Retire handles by clearing the slots, not by bumping in place: generations
    // are handed out from a monotonic counter and never reused, so a handle kept
    // across a reset fails its check even when a new slot lands at the same
    // index (rendergraph.md §7 stale-handle rule). Pruning also keeps rebuilt
    // frames identical for the topology hash (§8.3 cache key) and bounds growth.
    m_Textures.clear();
    m_Buffers.clear();
    m_TextureIndices.clear();
    m_BufferIndices.clear();
}

// ─── Topology hash ──────────────────────────────────────────────────────────
// Sum of per-resource material hashes. Order-independent by construction, so
// declaration order changes alone do not invalidate the cache (the builder's
// pass-order hash covers ordering, rendergraph.md §8.3).

namespace
{
u64 CombineInto(u64 a, u64 b)
{
    a ^= b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2);
    return a;
}
} // namespace

u64 RenderGraphResources::ComputeTopologyHash() const
{
    u64 hash = 0xcbf29ce484222325ull;
    for (const RGTextureSlot& slot : m_Textures)
    {
        hash = CombineInto(hash, slot.imported ? slot.import.Hash() : slot.desc.Hash());
        hash = CombineInto(hash, slot.imported ? 1u : 0u);
    }
    for (const RGBufferSlot& slot : m_Buffers)
    {
        hash = CombineInto(hash, slot.imported ? slot.import.Hash() : slot.desc.Hash());
        hash = CombineInto(hash, slot.imported ? 1u : 0u);
    }
    return hash;
}

} // namespace UHE::RHI
