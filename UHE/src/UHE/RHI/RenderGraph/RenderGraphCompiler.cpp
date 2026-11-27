#include "uhepch.h"
#include "RenderGraphCompiler.h"
#include <algorithm>
#include <map>
#include <queue>
#include <tuple>

namespace UHE::RHI
{

namespace
{

// ─── Stage/access derivation (the §8.3 "minimal-layout matrix") ─────────────
// Per use: which ImageState the pass needs the texture in, plus the stage/access
// pair that guards it. Undefined appears only as the initial state of freshly
// allocated memory — never between two defined states (the formal fix for the
// historical oldLayout = Undefined hardcode, rendergraph.md §8.3).

struct RGUseRequirement
{
    ImageState state = ImageState::Undefined;
    Stage stage = Stage::None;
    Access access = Access::None;
};

/// @brief A guard that wrote: whatever touches the resource next must wait for it.
bool IsWriteAccess(Access access)
{
    return access == Access::ColorWrite || access == Access::DepthWrite || access == Access::ShaderWrite ||
           access == Access::TransferWrite || access == Access::MemoryWrite;
}

RGUseRequirement TextureReadRequirement(RGPassType passType)
{
    if (passType == RGPassType::Transfer)
        return {ImageState::TransferSrc, Stage::Transfer, Access::TransferRead};
    if (passType == RGPassType::Compute)
        return {ImageState::ShaderRead, Stage::Compute, Access::ShaderRead};
    return {ImageState::ShaderRead, Stage::Fragment, Access::ShaderRead};
}

RGUseRequirement TextureWriteRequirement(RGPassType passType, bool isDepth)
{
    if (isDepth)
        return {ImageState::DepthAttachment, Stage::DepthTests, Access::DepthWrite};
    if (passType == RGPassType::Transfer)
        return {ImageState::TransferDst, Stage::Transfer, Access::TransferWrite};
    if (passType == RGPassType::Compute)
        return {ImageState::Storage, Stage::Compute, Access::ShaderWrite};
    return {ImageState::ColorAttachment, Stage::ColorOutput, Access::ColorWrite};
}

// Buffer "state" does not exist in Vulkan — only the guarding stage/access.
RGUseRequirement BufferReadRequirement(RGPassType passType)
{
    if (passType == RGPassType::Transfer)
        return {ImageState::Undefined, Stage::Transfer, Access::TransferRead};
    if (passType == RGPassType::Compute)
        return {ImageState::Undefined, Stage::Compute, Access::ShaderRead};
    // Graphics-pass buffer reads feed vertex input / uniforms.
    return {ImageState::Undefined, Stage::Vertex, Access::ShaderRead};
}

RGUseRequirement BufferWriteRequirement(RGPassType passType)
{
    if (passType == RGPassType::Transfer)
        return {ImageState::Undefined, Stage::Transfer, Access::TransferWrite};
    return {ImageState::Undefined, Stage::Compute, Access::ShaderWrite};
}

// Guards for the transition INTO a finalState (the exit contract). The dst side
// models the external consumer; presentation itself adds no access.
RGUseRequirement FinalStateRequirement(ImageState state)
{
    switch (state)
    {
        case ImageState::ShaderRead:
            return {state, Stage::Fragment, Access::ShaderRead};
        case ImageState::TransferSrc:
            return {state, Stage::Transfer, Access::TransferRead};
        case ImageState::TransferDst:
            return {state, Stage::Transfer, Access::TransferWrite};
        case ImageState::Storage:
            return {state, Stage::Compute, Access::ShaderWrite};
        case ImageState::Present:
        default:
            return {state, Stage::BottomOfPipe, Access::None};
    }
}

using RGTexReqMap = std::map<u32, RGUseRequirement>; // texture slot index → use
using RGBufReqMap = std::map<u32, RGUseRequirement>; // buffer slot index → use

// Collect what each pass needs per resource, and validate stage-1 use
// conflicts (one resource required in two different states by one pass).
struct RGPassRequirements
{
    RGTexReqMap textures;
    RGBufReqMap buffers;
    std::map<u32, RGTextureHandle> textureHandles; // slot → representative handle
    std::map<u32, RGBufferHandle> bufferHandles;
};

RGPassRequirements CollectRequirements(const RGPassSpec& spec, std::vector<RGValidationError>& errors)
{
    RGPassRequirements reqs;

    // Writes first so the produced state wins; a read of the same resource in
    // the same pass then surfaces as a state conflict (a read+write of one
    // texture in one pass is a hazard the graph does not express).
    for (const RGResourceAccess& access : spec.writes)
    {
        if (access.isTexture)
        {
            const bool isDepth = spec.hasDepth && spec.depth.texture == access.texture;
            const RGUseRequirement req = TextureWriteRequirement(spec.type, isDepth);
            const auto [it, inserted] = reqs.textures.emplace(access.texture.index, req);
            if (!inserted && it->second.state != req.state)
            {
                errors.push_back({RGErrorType::PassStateConflict, spec.name, "", ""});
                errors.back().message = "Texture required in two different states by one pass";
            }
            reqs.textureHandles.emplace(access.texture.index, access.texture);
        }
        else
        {
            reqs.buffers.emplace(access.buffer.index, BufferWriteRequirement(spec.type));
            reqs.bufferHandles.emplace(access.buffer.index, access.buffer);
        }
    }

    for (const RGResourceAccess& access : spec.reads)
    {
        if (access.isTexture)
        {
            const RGUseRequirement req = TextureReadRequirement(spec.type);
            const auto [it, inserted] = reqs.textures.emplace(access.texture.index, req);
            if (!inserted && it->second.state != req.state)
            {
                errors.push_back({RGErrorType::PassStateConflict, spec.name, "", ""});
                errors.back().message = "Texture required in two different states by one pass";
            }
            reqs.textureHandles.emplace(access.texture.index, access.texture);
        }
        else
        {
            reqs.buffers.emplace(access.buffer.index, BufferReadRequirement(spec.type));
            reqs.bufferHandles.emplace(access.buffer.index, access.buffer);
        }
    }

    return reqs;
}

} // namespace

// ─── Compile ────────────────────────────────────────────────────────────────

RGCompileResult RenderGraphCompiler::Compile(const std::vector<RGPassSpec>& passes,
                                             const RenderGraphResources& resources)
{
    RGCompileResult result;
    RGCompiledFrame& frame = result.frame;
    std::vector<RGValidationError>& errors = result.errors;

    frame.declaredPassCount = static_cast<u32>(passes.size());
    frame.topologyHash = resources.ComputeTopologyHash(); // refined by caller with pass structure

    // ── Stage 1: validate use conflicts, drop nameless placeholders ─────────
    std::vector<bool> passDropped(passes.size(), false);
    std::vector<RGPassRequirements> reqsByPass;
    reqsByPass.reserve(passes.size());
    for (const RGPassSpec& spec : passes)
    {
        if (spec.name.empty())
        {
            errors.push_back({RGErrorType::UnsubmittedPass, "", "", "Compiler found an unsubmitted pass builder"});
            passDropped[reqsByPass.size()] = true;
        }
        reqsByPass.push_back(CollectRequirements(spec, errors));
    }

    // ── Stage 2: topological sort (Kahn's, version edges) ───────────────────
    // Producer of (resource, version) → declaring pass. With declaration-time
    // version binding (§7) the declaration order is already topological; the
    // sort verifies it and gives the queue-batching stages a real edge set.
    // Ties break toward declaration order, so a single-queue frame compiles to
    // exactly the declared sequence.
    std::map<std::tuple<bool, u32, u32>, u32> producerOf; // (isTex, slot, version) → decl pass
    for (u32 slot = 0; slot < resources.Textures().size(); ++slot)
        for (const RGResourceVersion& v : resources.Textures()[slot].versionLog)
            producerOf.emplace(std::make_tuple(true, slot, v.version), v.passIndex);
    for (u32 slot = 0; slot < resources.Buffers().size(); ++slot)
        for (const RGResourceVersion& v : resources.Buffers()[slot].versionLog)
            producerOf.emplace(std::make_tuple(false, slot, v.version), v.passIndex);

    // An attachment with LoadOp::Load reads what the previous version holds,
    // though it is declared as a write: the pass that produced that version is
    // a dependency like any reader's producer. Without the edge, a pass whose
    // only consumer loads its output (a copy the water pass draws over) was
    // culled and the loading pass read an uninitialized image.
    const auto loadedProducers = [&](u32 p)
    {
        std::vector<u32> producers;
        const auto loads = [&](RGTextureHandle texture)
        {
            for (const RGColorAttachment& color : passes[p].colors)
                if (color.texture.index == texture.index && color.load == LoadOp::Load)
                    return true;
            return passes[p].hasDepth && passes[p].depth.texture.index == texture.index &&
                   passes[p].depth.load == LoadOp::Load;
        };
        for (const RGResourceAccess& access : passes[p].writes)
        {
            if (!access.isTexture || access.version == 0 || !loads(access.texture))
                continue;
            const auto it = producerOf.find(std::make_tuple(true, access.texture.index, access.version - 1));
            if (it != producerOf.end() && it->second != p)
                producers.push_back(it->second);
        }
        return producers;
    };

    std::vector<u32> indegree(passes.size(), 0);
    std::vector<std::vector<u32>> dependents(passes.size()); // producer → readers
    for (u32 p = 0; p < passes.size(); ++p)
    {
        if (passDropped[p])
            continue;
        for (const RGResourceAccess& access : passes[p].reads)
        {
            const u32 slot = access.isTexture ? access.texture.index : access.buffer.index;
            const auto it = producerOf.find(std::make_tuple(access.isTexture, slot, access.version));
            if (it == producerOf.end() || it->second == p)
                continue; // imported-at-v0 or self
            dependents[it->second].push_back(p);
            ++indegree[p];
        }
        for (const u32 producer : loadedProducers(p))
        {
            dependents[producer].push_back(p);
            ++indegree[p];
        }
    }

    std::priority_queue<u32, std::vector<u32>, std::greater<u32>> ready;
    for (u32 p = 0; p < passes.size(); ++p)
        if (!passDropped[p] && indegree[p] == 0)
            ready.push(p);

    std::vector<u32> topoOrder;
    topoOrder.reserve(passes.size());
    while (!ready.empty())
    {
        const u32 p = ready.top();
        ready.pop();
        topoOrder.push_back(p);
        for (const u32 dependent : dependents[p])
            if (--indegree[dependent] == 0)
                ready.push(dependent);
    }

    u32 liveDeclCount = 0;
    for (u32 p = 0; p < passes.size(); ++p)
        liveDeclCount += passDropped[p] ? 0 : 1;
    if (topoOrder.size() != liveDeclCount)
    {
        errors.push_back(
            {RGErrorType::PassCycle, "", "", "Pass dependency cycle detected (Kahn's queue emptied early)"});
        return result; // a cycle makes every later stage meaningless
    }

    // ── Stage 3: cull — backward-reachable from import roots ────────────────
    // Roots are passes touching an imported resource (read or write). A pass is
    // live if a root transitively depends on it (producer edges only — culling
    // consumers of a culled pass is the point).
    const auto touchesImport = [&](u32 p)
    {
        const RGPassRequirements& reqs = reqsByPass[p];
        for (const auto& [slot, req] : reqs.textures)
            if (slot < resources.Textures().size() && resources.Textures()[slot].imported)
                return true;
        for (const auto& [slot, req] : reqs.buffers)
            if (slot < resources.Buffers().size() && resources.Buffers()[slot].imported)
                return true;
        return false;
    };

    // Read edges: consumer → producer (for backward reachability).
    std::vector<std::vector<u32>> producersOf(passes.size());
    for (u32 p = 0; p < passes.size(); ++p)
    {
        for (const RGResourceAccess& access : passes[p].reads)
        {
            const u32 slot = access.isTexture ? access.texture.index : access.buffer.index;
            const auto it = producerOf.find(std::make_tuple(access.isTexture, slot, access.version));
            if (it != producerOf.end() && it->second != p)
                producersOf[p].push_back(it->second);
        }
        for (const u32 producer : loadedProducers(p))
            producersOf[p].push_back(producer);
    }

    std::vector<bool> live(passes.size(), false);
    std::vector<u32> worklist;
    for (const u32 p : topoOrder)
    {
        if (touchesImport(p) && !live[p])
        {
            live[p] = true;
            worklist.push_back(p);
        }
    }
    while (!worklist.empty())
    {
        const u32 p = worklist.back();
        worklist.pop_back();
        for (const u32 producer : producersOf[p])
        {
            if (!live[producer])
            {
                live[producer] = true;
                worklist.push_back(producer);
            }
        }
    }

    for (u32 p = 0; p < passes.size(); ++p)
        if (passDropped[p] || !live[p])
            if (!passes[p].name.empty() && !live[p])
                frame.culledPasses.push_back(passes[p].name);

    // ── Emit compiled passes (topological, live only) ───────────────────────
    std::vector<u32> compiledIdOfDecl(passes.size(), RG::kInvalidIndex);
    for (const u32 p : topoOrder)
    {
        if (!live[p])
            continue;
        RGCompiledPass compiled;
        compiled.id = static_cast<u32>(frame.passes.size());
        compiled.passIndex = p;
        compiled.name = passes[p].name;
        compiled.type = passes[p].type;
        compiled.queue = passes[p].queue;
        compiledIdOfDecl[p] = compiled.id;
        frame.passes.push_back(std::move(compiled));
    }

    // Dependencies: version-edge producers, restricted to live passes.
    for (RGCompiledPass& compiled : frame.passes)
    {
        for (const u32 producer : producersOf[compiled.passIndex])
        {
            if (live[producer])
                compiled.dependencies.push_back(compiledIdOfDecl[producer]);
        }
        std::sort(compiled.dependencies.begin(), compiled.dependencies.end());
        compiled.dependencies.erase(std::unique(compiled.dependencies.begin(), compiled.dependencies.end()),
                                    compiled.dependencies.end());
    }

    // ── Stage 4: lifetimes, in compiled order ───────────────────────────────
    const auto LifetimeKey = [](bool isTexture, u32 slot) { return std::make_pair(isTexture, slot); };
    std::map<std::pair<bool, u32>, u32> lifetimeIndex;
    const auto TouchLifetime = [&](bool isTexture, u32 slot, u32 compiledId, bool isWrite)
    {
        const auto [it, inserted] =
            lifetimeIndex.emplace(LifetimeKey(isTexture, slot), static_cast<u32>(frame.lifetimes.size()));
        if (inserted)
        {
            RGCompiledResourceLifetime lifetime;
            lifetime.isTexture = isTexture;
            lifetime.firstUsePass = compiledId;
            lifetime.lastUsePass = compiledId;
            if (isTexture)
            {
                const RGTextureSlot& textureSlot = resources.Textures()[slot];
                lifetime.texture = RGTextureHandle{slot, textureSlot.generation};
                lifetime.name = textureSlot.name;
                lifetime.imported = textureSlot.imported;
                lifetime.versionCount = textureSlot.currentVersion;
            }
            else
            {
                const RGBufferSlot& bufferSlot = resources.Buffers()[slot];
                lifetime.buffer = RGBufferHandle{slot, bufferSlot.generation};
                lifetime.name = bufferSlot.name;
                lifetime.imported = bufferSlot.imported;
                lifetime.versionCount = bufferSlot.currentVersion;
            }
            if (isWrite)
                lifetime.firstWritePass = compiledId;
            frame.lifetimes.push_back(std::move(lifetime));
            return;
        }
        RGCompiledResourceLifetime& lifetime = frame.lifetimes[it->second];
        lifetime.lastUsePass = std::max(lifetime.lastUsePass, compiledId);
        if (isWrite && lifetime.firstWritePass == RG::kInvalidIndex)
            lifetime.firstWritePass = compiledId;
    };

    for (const RGCompiledPass& compiled : frame.passes)
    {
        const RGPassRequirements& reqs = reqsByPass[compiled.passIndex];
        for (const auto& [slot, handle] : reqs.textureHandles)
            TouchLifetime(true, slot, compiled.id, reqs.textures.count(slot) > 0);
        for (const auto& [slot, handle] : reqs.bufferHandles)
            TouchLifetime(false, slot, compiled.id, reqs.buffers.count(slot) > 0);
    }

    // ── Stages 6–7: state tracking + barrier synthesis ──────────────────────
    // Layout tracker: current state + the guard that produced it. Freshly
    // allocated (transient) textures start Undefined with a TopOfPipe guard;
    // imports start at their declared initial state.
    struct RGTexTrack
    {
        ImageState state = ImageState::Undefined;
        Stage stage = Stage::TopOfPipe;
        Access access = Access::None;
        bool used = false; // false = never touched on the GPU yet
    };
    std::map<u32, RGTexTrack> texTracks;
    for (u32 slot = 0; slot < resources.Textures().size(); ++slot)
    {
        const RGTextureSlot& textureSlot = resources.Textures()[slot];
        RGTexTrack track;
        track.state = textureSlot.imported ? textureSlot.desc.initialState : ImageState::Undefined;
        texTracks.emplace(slot, track);
    }
    struct RGBufTrack
    {
        Stage stage = Stage::None; // None = no GPU use yet
        Access access = Access::None;
    };
    std::map<u32, RGBufTrack> bufTracks;

    for (RGCompiledPass& compiled : frame.passes)
    {
        const RGPassSpec& spec = passes[compiled.passIndex];
        const RGPassRequirements& reqs = reqsByPass[compiled.passIndex];

        for (const auto& [slot, req] : reqs.textures)
        {
            RGTexTrack& track = texTracks[slot];
            if (!track.used || track.state != req.state)
            {
                // Layout transition, or the first use this frame. A first use
                // waits on everything submitted before (all stages, all
                // writes): a pooled image was written by the previous frame,
                // the swapchain image by the acquire, and a TopOfPipe source
                // ordered neither (sync validation: WRITE_AFTER_WRITE).
                compiled.preImageBarriers.push_back({reqs.textureHandles.at(slot), track.state, req.state,
                                                     track.used ? track.stage : Stage::AllCommands, req.stage,
                                                     track.used ? track.access : Access::MemoryWrite, req.access});
                track.state = req.state;
                track.stage = req.stage;
                track.access = req.access;
                track.used = true;
            }
            else if (track.stage != req.stage || track.access != req.access || IsWriteAccess(track.access))
            {
                // Same layout, different guard — access-only barrier. Also
                // after any write: two passes rendering into one attachment in
                // a row are separate rendering scopes, and nothing orders the
                // second's load and writes after the first's without this.
                compiled.preImageBarriers.push_back({reqs.textureHandles.at(slot), track.state, req.state, track.stage,
                                                     req.stage, track.access, req.access});
                track.stage = req.stage;
                track.access = req.access;
            }
            // Same state + same guard after a read: no barrier (layout
            // minimality — a texture read as SRV by two passes transitions once).
        }

        for (const auto& [slot, req] : reqs.buffers)
        {
            RGBufTrack& track = bufTracks[slot];
            if (track.stage == Stage::None)
            {
                track.stage = req.stage;
                track.access = req.access;
            }
            else if (track.stage != req.stage || track.access != req.access)
            {
                compiled.preBufferBarriers.push_back(
                    {reqs.bufferHandles.at(slot), track.stage, req.stage, track.access, req.access});
                track.stage = req.stage;
                track.access = req.access;
            }
        }
    }

    // ── finalState exits: attach to the last compiled pass that used the ────
    // texture (or the last pass overall when the consumer was culled).
    for (auto& [slot, track] : texTracks)
    {
        if (!track.used)
            continue;
        const RGTextureSlot& textureSlot = resources.Textures()[slot];
        const ImageState finalState = textureSlot.desc.finalState;
        if (finalState == ImageState::Undefined || finalState == track.state)
            continue;

        const RGUseRequirement dst = FinalStateRequirement(finalState);
        RGCompiledImageBarrier barrier{RGTextureHandle{slot, textureSlot.generation},
                                       track.state,
                                       finalState,
                                       track.stage,
                                       dst.stage,
                                       track.access,
                                       dst.access};

        u32 targetId = RG::kInvalidIndex;
        for (auto it = frame.passes.rbegin(); it != frame.passes.rend(); ++it)
        {
            if (reqsByPass[it->passIndex].textures.count(slot) > 0)
            {
                targetId = it->id;
                break;
            }
        }
        if (targetId == RG::kInvalidIndex && !frame.passes.empty())
            targetId = frame.passes.back().id;
        if (targetId != RG::kInvalidIndex)
            frame.passes[targetId].postImageBarriers.push_back(barrier);
    }

    return result;
}

} // namespace UHE::RHI
