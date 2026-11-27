#pragma once
#include <algorithm> // std::find (implied-write bookkeeping)
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"
#include "UHE/RHI/RenderGraph/RenderGraphResources.h"
#include "UHE/RHI/RenderGraph/RenderGraphTypes.h"

// Render graph builder (docs/architecture/rendergraph.md §8.1, step 1 slice).
//
// Step 1 covers declaration + validation + raw-graph dump only. The execute
// lambda is *stored, not called* — the executor (§8.5) invokes it in a later
// milestone. Compile()/Execute() are deliberately absent: step 2 adds them with
// the compiler, so no half-wired callsites exist.

namespace UHE::RHI
{

class RenderGraphBuilder;

// Fluent per-pass surface. AddPass() returns a reference to a builder stored
// inside RenderGraphBuilder (std::deque — references stay valid), so both
// usage styles from rendergraph.md §8.1 work:
//
//   auto& pass = rg.AddPass("GBuffer");
//   pass.Write(albedo).Color({...}).Execute([&](RGPassContext&){ ... });
//
//   rg.AddPass("Lighting").Read(albedo).Write(swap).Execute(fn);
//
// Execute() auto-publishes the pass (it is the natural end of a declaration);
// a pass without an execute callback must call Submit() explicitly. Forgetting
// both leaves a nameless placeholder that Validate() reports.
class UHE_API RGPassBuilder
{
public:
    RGPassBuilder(RenderGraphBuilder& builder, RGPassSpec spec, u32 passIndex)
        : m_Builder(builder), m_Spec(std::move(spec)), m_PassIndex(passIndex)
    {
    }

    RGPassBuilder(const RGPassBuilder&) = delete;
    RGPassBuilder& operator=(const RGPassBuilder&) = delete;

    [[nodiscard]] RGPassBuilder& Color(const RGColorAttachment& attachment);
    [[nodiscard]] RGPassBuilder& Depth(const RGDepthAttachment& attachment);
    [[nodiscard]] RGPassBuilder& Read(RGTextureHandle texture);
    [[nodiscard]] RGPassBuilder& Read(RGBufferHandle buffer);
    [[nodiscard]] RGPassBuilder& Write(RGTextureHandle texture);
    [[nodiscard]] RGPassBuilder& Write(RGBufferHandle buffer);
    [[nodiscard]] RGPassBuilder& Queue(RGQueue queue);
    // Stored on the spec and AUTO-PUBLISHES the pass (see class comment). The
    // return value may be freely discarded — it is the end of the chain.
    RGPassBuilder& Execute(RGPassExecuteFn&& fn);

    // Publishes the spec into the graph. After this the builder may not be
    // used again (debug assert). Not needed when Execute() ends the chain.
    void Submit();

private:
    // True when this pass already wrote the texture — either explicitly via
    // Write() or implicitly via an attachment declaration (§7: attaching a
    // target IS producing it).
    [[nodiscard]] bool WroteTexture(RGTextureHandle texture) const;

    RenderGraphBuilder& m_Builder;
    RGPassSpec m_Spec;
    u32 m_PassIndex = RG::kInvalidIndex;
    bool m_Submitted = false;
    // Textures whose write was implied by a Color()/Depth() attachment of this
    // pass. Lets an explicit Write() of the same texture no-op without masking
    // genuine DoubleWrite errors (two explicit writes still report).
    std::vector<RGTextureHandle> m_AttachmentImpliedWrites;
    friend class RenderGraphBuilder;
};

class UHE_API RenderGraphBuilder
{
public:
    RenderGraphBuilder() = default;
    RenderGraphBuilder(const RenderGraphBuilder&) = delete;
    RenderGraphBuilder& operator=(const RenderGraphBuilder&) = delete;
    ~RenderGraphBuilder() = default;

    // Passes through to the registry; see RenderGraphResources for rules.
    RGTextureHandle CreateTexture(const RGTextureDesc& desc);
    RGBufferHandle CreateBuffer(const RGBufferDesc& desc);
    RGTextureHandle ImportTexture(const std::string& name, const RGImportedDesc& identity, ImageState initialState,
                                  ImageState finalState);
    RGBufferHandle ImportBuffer(const std::string& name, const RGImportedDesc& identity);

    // Begins a pass. Name is required and non-empty (debug assert + validation).
    // Returns a reference into the internal deque; valid until Reset().
    RGPassBuilder& AddPass(const std::string& name, RGPassType type = RGPassType::Graphics,
                           RGQueue queue = RGQueue::Graphics);

    // ─── Finalization ───

    // Applies §7 rules: read-binds-to-latest-version, single writer per version,
    // read-before-write. Collects all errors; never throws.
    [[nodiscard]] std::vector<RGValidationError> Validate() const;

    // Registry snapshot for the dump and (later) the compiler.
    [[nodiscard]] const RenderGraphResources& Resources() const { return m_Resources; }

    [[nodiscard]] const std::vector<RGPassSpec>& Passes() const { return m_Passes; }

    void Reset();
    [[nodiscard]] u64 ComputeTopologyHash() const;

    // ─── Builder internals (RGPassBuilder is a friend) ───

    // Validates the handle, then versions the resource and records the access.
    // Returns false and appends a validation error on a stale handle.
    bool RecordTextureWrite(RGPassSpec& spec, RGTextureHandle handle, u32 passIndex);
    bool RecordBufferWrite(RGPassSpec& spec, RGBufferHandle handle, u32 passIndex);
    bool RecordTextureRead(RGPassSpec& spec, RGTextureHandle handle, u32 passIndex);
    bool RecordBufferRead(RGPassSpec& spec, RGBufferHandle handle, u32 passIndex);

    void PushError(RGValidationError error) { m_Errors.push_back(std::move(error)); }

private:
    friend class RGPassBuilder;

    RenderGraphResources m_Resources;
    std::vector<RGPassSpec> m_Passes;
    std::deque<RGPassBuilder> m_PassBuilders; // stable references for AddPass
    mutable std::vector<RGValidationError> m_Errors;
};

} // namespace UHE::RHI
