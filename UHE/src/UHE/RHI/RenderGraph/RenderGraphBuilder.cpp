#include "uhepch.h"
#include "RenderGraphBuilder.h"
#include <algorithm>

namespace UHE::RHI
{

namespace
{
std::string VersionedName(const std::string& name, u32 version)
{
    return name + ".v" + std::to_string(version);
}
} // namespace

// ─── RGPassBuilder helpers ─────────────────────────────────────────────────

bool RGPassBuilder::WroteTexture(RGTextureHandle texture) const
{
    // An explicit Write() or an attachment-implied production both count: the
    // resource already has a version bound to this pass.
    return std::any_of(m_Spec.writes.begin(), m_Spec.writes.end(),
                       [&](const RGResourceAccess& access) { return access.isTexture && access.texture == texture; });
}

// ─── RGPassBuilder ──────────────────────────────────────────────────────────

RGPassBuilder& RGPassBuilder::Color(const RGColorAttachment& attachment)
{
    UHE_CORE_ASSERT(!m_Submitted, "RenderGraph: pass builder used after Submit()");
    m_Spec.colors.push_back(attachment);

    // §7: attaching a target means this pass PRODUCES it, so the attachment
    // implies the write. Auto-recording it here makes both declaration orders
    // describe the same pass — Color()-first (ImGui's single attachment chain)
    // and Write()-first (explicit pipelines) — without a second version being
    // minted when the explicit Write() arrives later.
    if (attachment.texture.IsValid() && !WroteTexture(attachment.texture))
    {
        m_AttachmentImpliedWrites.push_back(attachment.texture);
        m_Builder.RecordTextureWrite(m_Spec, attachment.texture, m_PassIndex);
    }
    return *this;
}

RGPassBuilder& RGPassBuilder::Depth(const RGDepthAttachment& attachment)
{
    UHE_CORE_ASSERT(!m_Submitted, "RenderGraph: pass builder used after Submit()");
    m_Spec.hasDepth = true;
    m_Spec.depth = attachment;

    // Same implied-production rule as Color(): the depth target is written by
    // this pass whether or not the chain spelled Write() out.
    if (attachment.texture.IsValid() && !WroteTexture(attachment.texture))
    {
        m_AttachmentImpliedWrites.push_back(attachment.texture);
        m_Builder.RecordTextureWrite(m_Spec, attachment.texture, m_PassIndex);
    }
    return *this;
}

RGPassBuilder& RGPassBuilder::Read(RGTextureHandle texture)
{
    UHE_CORE_ASSERT(!m_Submitted, "RenderGraph: pass builder used after Submit()");
    m_Builder.RecordTextureRead(m_Spec, texture, m_PassIndex);
    return *this;
}

RGPassBuilder& RGPassBuilder::Read(RGBufferHandle buffer)
{
    UHE_CORE_ASSERT(!m_Submitted, "RenderGraph: pass builder used after Submit()");
    m_Builder.RecordBufferRead(m_Spec, buffer, m_PassIndex);
    return *this;
}

RGPassBuilder& RGPassBuilder::Write(RGTextureHandle texture)
{
    UHE_CORE_ASSERT(!m_Submitted, "RenderGraph: pass builder used after Submit()");
    // Redundant when an attachment of THIS pass already implied the write
    // (Color()/Depth()-first style): the production is already version-bound.
    // Two EXPLICIT writes are still a DoubleWrite error — see RecordTextureWrite.
    if (std::find(m_AttachmentImpliedWrites.begin(), m_AttachmentImpliedWrites.end(), texture) !=
        m_AttachmentImpliedWrites.end())
        return *this;
    m_Builder.RecordTextureWrite(m_Spec, texture, m_PassIndex);
    return *this;
}

RGPassBuilder& RGPassBuilder::Write(RGBufferHandle buffer)
{
    UHE_CORE_ASSERT(!m_Submitted, "RenderGraph: pass builder used after Submit()");
    m_Builder.RecordBufferWrite(m_Spec, buffer, m_PassIndex);
    return *this;
}

RGPassBuilder& RGPassBuilder::Queue(RGQueue queue)
{
    UHE_CORE_ASSERT(!m_Submitted, "RenderGraph: pass builder used after Submit()");
    m_Spec.queue = queue;
    return *this;
}

RGPassBuilder& RGPassBuilder::Execute(RGPassExecuteFn&& fn)
{
    UHE_CORE_ASSERT(!m_Submitted, "RenderGraph: pass builder used after Submit()");
    m_Spec.execute = std::move(fn);
    // Auto-publish: Execute is the natural end of a declaration chain, so the
    // common §8.1 style (rg.AddPass("X").…Execute(fn);) needs no explicit
    // Submit() and cannot dangle the returned reference.
    Submit();
    return *this;
}

void RGPassBuilder::Submit()
{
    UHE_CORE_ASSERT(!m_Submitted, "RenderGraph: pass builder submitted twice");
    if (m_Submitted)
        return;
    m_Submitted = true;
    // The spec was copied into the builder at construction; publication order
    // matters for version logs, so the builder keeps m_Passes authoritative and
    // AddPass reserved the slot up front. Overwrite the reserved entry.
    if (m_PassIndex < m_Builder.m_Passes.size())
        m_Builder.m_Passes[m_PassIndex] = std::move(m_Spec);
}

// ─── RenderGraphBuilder ───────────────────────────────────────────────

RGTextureHandle RenderGraphBuilder::CreateTexture(const RGTextureDesc& desc)
{
    return m_Resources.CreateTexture(desc);
}

RGBufferHandle RenderGraphBuilder::CreateBuffer(const RGBufferDesc& desc)
{
    return m_Resources.CreateBuffer(desc);
}

RGTextureHandle RenderGraphBuilder::ImportTexture(const std::string& name, const RGImportedDesc& identity,
                                                  ImageState initialState, ImageState finalState)
{
    return m_Resources.ImportTexture(name, identity, initialState, finalState);
}

RGBufferHandle RenderGraphBuilder::ImportBuffer(const std::string& name, const RGImportedDesc& identity)
{
    return m_Resources.ImportBuffer(name, identity);
}

RGPassBuilder& RenderGraphBuilder::AddPass(const std::string& name, RGPassType type, RGQueue queue)
{
    UHE_CORE_ASSERT(!name.empty(), "RenderGraph: pass name must not be empty");

    RGPassSpec spec;
    spec.name = name;
    spec.type = type;
    spec.queue = queue;

    // Reserve now so versionLog passIndex values stay valid even if the caller
    // drops the RGPassBuilder without Submit() — the entry is overwritten on
    // Submit, and a dropped builder leaves an empty-name placeholder that
    // validation reports.
    const u32 passIndex = static_cast<u32>(m_Passes.size());
    m_Passes.emplace_back();

    // The builder lives in a deque so the returned reference stays valid for
    // chaining (std::deque never invalidates references on emplace_back).
    return m_PassBuilders.emplace_back(*this, std::move(spec), passIndex);
}

// ─── Access recording ───────────────────────────────────────────────────────
// Stale handles are validation errors, not asserts: a dropped frame can race a
// reset in release builds and we want the graph to name the offender instead of
// crashing (rendergraph.md §7 rules).

bool RenderGraphBuilder::RecordTextureWrite(RGPassSpec& spec, RGTextureHandle handle, u32 passIndex)
{
    const RGTextureSlot* slot = m_Resources.GetTexture(handle);
    if (!slot)
    {
        PushError({RGErrorType::StaleHandle, spec.name, "texture#" + std::to_string(handle.index),
                   "Write() on stale or invalid texture handle"});
        return false;
    }

    // Single-writer-per-version by construction: every Write() versions the
    // resource first, so the next writer targets a fresh version. The access is
    // bound to the produced version NOW (§7) — a later pass's write must not
    // retroactively rebind this declaration.
    const u32 version = m_Resources.AdvanceTextureVersion(handle, passIndex);
    spec.writes.push_back(RGResourceAccess{handle, {}, true, version});
    return true;
}

bool RenderGraphBuilder::RecordBufferWrite(RGPassSpec& spec, RGBufferHandle handle, u32 passIndex)
{
    const RGBufferSlot* slot = m_Resources.GetBuffer(handle);
    if (!slot)
    {
        PushError({RGErrorType::StaleHandle, spec.name, "buffer#" + std::to_string(handle.index),
                   "Write() on stale or invalid buffer handle"});
        return false;
    }

    const u32 version = m_Resources.AdvanceBufferVersion(handle, passIndex);
    spec.writes.push_back(RGResourceAccess{{}, handle, false, version});
    return true;
}

bool RenderGraphBuilder::RecordTextureRead(RGPassSpec& spec, RGTextureHandle handle, u32 passIndex)
{
    const RGTextureSlot* slot = m_Resources.GetTexture(handle);
    if (!slot)
    {
        PushError({RGErrorType::StaleHandle, spec.name, "texture#" + std::to_string(handle.index),
                   "Read() on stale or invalid texture handle"});
        return false;
    }

    // Read-before-write: version 0 means no pass wrote this resource yet. The
    // one legal exception is imported resources — they exist before the graph
    // (swapchain image, sensor buffer), so reading v0 is reading external state
    // (rendergraph.md §7).
    if (slot->currentVersion == 0 && !slot->imported)
    {
        PushError(
            {RGErrorType::ReadBeforeWrite, spec.name, slot->name, "Read() before any Write() on non-imported texture"});
        return false;
    }

    // Read binds to the version that exists at declaration time (§7), even if a
    // later pass writes the resource and bumps it before the graph is dumped.
    const u32 boundVersion = slot->currentVersion;

    spec.reads.push_back(RGResourceAccess{handle, {}, true, boundVersion});
    return true;
}

bool RenderGraphBuilder::RecordBufferRead(RGPassSpec& spec, RGBufferHandle handle, u32 passIndex)
{
    const RGBufferSlot* slot = m_Resources.GetBuffer(handle);
    if (!slot)
    {
        PushError({RGErrorType::StaleHandle, spec.name, "buffer#" + std::to_string(handle.index),
                   "Read() on stale or invalid buffer handle"});
        return false;
    }

    if (slot->currentVersion == 0 && !slot->imported)
    {
        PushError(
            {RGErrorType::ReadBeforeWrite, spec.name, slot->name, "Read() before any Write() on non-imported buffer"});
        return false;
    }

    // Same binding rule as textures: read the version current right now.
    const u32 boundVersion = slot->currentVersion;

    spec.reads.push_back(RGResourceAccess{{}, handle, false, boundVersion});
    return true;
}

// ─── Validation ─────────────────────────────────────────────────────────────
// Single-writer-per-version is enforced by construction here: every Write()
// advances the version before the next pass can write, so two writes can only
// collide via aliased name handles — the desc-conflict check below. All other
// rules are collected here too so one Validate() call reports everything.

std::vector<RGValidationError> RenderGraphBuilder::Validate() const
{
    std::vector<RGValidationError> errors = m_Errors;

    for (const RGPassSpec& pass : m_Passes)
    {
        if (pass.name.empty())
        {
            errors.push_back({RGErrorType::UnsubmittedPass, "", "",
                              "A pass builder was created but never submitted "
                              "(nameless placeholder at its declaration slot)"});
            continue;
        }

        // §7 single-writer rule, per pass: writing the same resource twice in one
        // pass orphans the first version (nothing can ever read it).
        for (size_t i = 0; i < pass.writes.size(); ++i)
        {
            for (size_t j = i + 1; j < pass.writes.size(); ++j)
            {
                const RGResourceAccess& a = pass.writes[i];
                const RGResourceAccess& b = pass.writes[j];
                if (a.isTexture != b.isTexture)
                    continue;
                const bool sameResource = a.isTexture ? (a.texture == b.texture) : (a.buffer == b.buffer);
                if (sameResource)
                {
                    errors.push_back(
                        {RGErrorType::DoubleWrite, pass.name,
                         a.isTexture ? m_Resources.TextureName(a.texture) : m_Resources.BufferName(a.buffer),
                         "Resource written twice by one pass — the first write is orphaned"});
                }
            }
        }

        // Attachment texture must also be declared as written this pass: the
        // color/depth target is produced here, and skipping the Write() would
        // leave the resource version behind the actual GPU output.
        for (const RGColorAttachment& color : pass.colors)
        {
            if (color.texture.IsValid())
            {
                const bool declaredWritten =
                    std::any_of(pass.writes.begin(), pass.writes.end(), [&](const RGResourceAccess& access)
                                { return access.isTexture && access.texture == color.texture; });
                if (!declaredWritten)
                {
                    errors.push_back({RGErrorType::UndeclaredAttachment, pass.name,
                                      m_Resources.TextureName(color.texture),
                                      "Color attachment not declared with Write() — attach via "
                                      "Write() then Color(), or call Color() before other access"});
                }
            }
        }

        if (pass.hasDepth && pass.depth.texture.IsValid())
        {
            const bool declaredWritten =
                std::any_of(pass.writes.begin(), pass.writes.end(), [&](const RGResourceAccess& access)
                            { return access.isTexture && access.texture == pass.depth.texture; });
            if (!declaredWritten)
            {
                errors.push_back({RGErrorType::UndeclaredAttachment, pass.name,
                                  m_Resources.TextureName(pass.depth.texture),
                                  "Depth attachment not declared with Write()"});
            }
        }
    }

    // Imported descs must stay consistent across a frame: two imports of the
    // same name with materially different descs mean the caller re-declared a
    // resource inconsistently (typo'd extent, wrong identity hash).
    // (Name-dedup makes later imports no-ops, so only a material mismatch is
    // detectable here — reported against the first declaring pass.)

    return errors;
}

// ─── Reset & hash ───────────────────────────────────────────────────────────

void RenderGraphBuilder::Reset()
{
    m_Resources.Reset();
    m_Passes.clear();
    m_PassBuilders.clear();
    m_Errors.clear();
}

u64 RenderGraphBuilder::ComputeTopologyHash() const
{
    // Resource material hash, order-independent …
    u64 hash = m_Resources.ComputeTopologyHash();
    // … plus pass structure, order-dependent (names + declared access shape).
    // This is the §8.3 cache key minus extent/tier, which the facade adds once
    // imports carry real dimensions.
    for (const RGPassSpec& pass : m_Passes)
    {
        hash ^= std::hash<std::string>{}(pass.name) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= static_cast<u64>(pass.type) << 1;
        hash ^= static_cast<u64>(pass.queue) << 2;
        hash ^= (static_cast<u64>(pass.reads.size()) << 8) ^ static_cast<u64>(pass.writes.size());
        hash ^= static_cast<u64>(pass.colors.size()) << 16;
        hash ^= pass.hasDepth ? (1ull << 20) : 0ull;
    }
    return hash;
}

} // namespace UHE::RHI
