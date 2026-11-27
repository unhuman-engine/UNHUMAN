#include "UHE/RHI/RenderGraph/RenderGraphCompileCache.h"

namespace UHE::RHI
{

namespace
{
void Push(std::vector<u64>& words, u64 value)
{
    words.push_back(value);
}

void PushTextureDesc(std::vector<u64>& words, const RGTextureDesc& desc)
{
    Push(words, static_cast<u64>(desc.format));
    Push(words, (static_cast<u64>(desc.width) << 32) | desc.height);
    Push(words, (static_cast<u64>(desc.depth) << 32) | desc.mipLevels);
    Push(words, (static_cast<u64>(desc.layers) << 32) | desc.sampleCount);
    Push(words, static_cast<u64>(desc.usage));
    Push(words, (static_cast<u64>(desc.initialState) << 8) | static_cast<u64>(desc.finalState));
}

void PushAccess(std::vector<u64>& words, const RGResourceAccess& access)
{
    const u32 index = access.isTexture ? access.texture.index : access.buffer.index;
    Push(words, (static_cast<u64>(access.isTexture) << 63) | (static_cast<u64>(index) << 32) | access.version);
}
} // namespace

RenderGraphCompileCache::Key RenderGraphCompileCache::BuildKey(const RenderGraphBuilder& builder,
                                                               const RGCompileOptions& options)
{
    Key key;
    std::vector<u64>& w = key.Words;
    const RenderGraphResources& resources = builder.Resources();
    Push(w, options.asyncCompute ? 1u : 0u); // the queue a pass lands on depends on it

    Push(w, resources.Textures().size());
    for (const RGTextureSlot& slot : resources.Textures())
    {
        key.Names.push_back(slot.name);
        Push(w, slot.imported ? 1u : 0u);
        PushTextureDesc(w, slot.desc);
        Push(w, slot.imported ? slot.import.Hash() : 0u);
        Push(w, slot.currentVersion);
    }
    Push(w, resources.Buffers().size());
    for (const RGBufferSlot& slot : resources.Buffers())
    {
        key.Names.push_back(slot.name);
        Push(w, slot.imported ? 1u : 0u);
        Push(w, slot.imported ? slot.import.Hash() : slot.desc.Hash());
        Push(w, slot.currentVersion);
    }

    Push(w, builder.Passes().size());
    for (const RGPassSpec& pass : builder.Passes())
    {
        key.Names.push_back(pass.name);
        Push(w, (static_cast<u64>(pass.type) << 8) | static_cast<u64>(pass.queue));
        Push(w, pass.hasDepth ? (1ull << 32) | pass.depth.texture.index : 0u);
        Push(w, pass.colors.size());
        for (const RGColorAttachment& color : pass.colors)
            Push(w, (static_cast<u64>(color.texture.index) << 32) | color.resolve.index);
        Push(w, pass.reads.size());
        for (const RGResourceAccess& access : pass.reads)
            PushAccess(w, access);
        Push(w, pass.writes.size());
        for (const RGResourceAccess& access : pass.writes)
            PushAccess(w, access);
    }
    return key;
}

RGCompileResult RenderGraphCompileCache::Compile(const RenderGraphBuilder& builder, const RGCompileOptions& options)
{
    Key key = BuildKey(builder, options);
    const RenderGraphResources& resources = builder.Resources();

    if (m_Valid && key == m_Key)
    {
        ++m_Hits;
        RGCompileResult result;
        result.frame = m_Frame;
        const auto texture = [&](RGTextureHandle& handle)
        {
            if (handle.index < resources.Textures().size())
                handle.generation = resources.Textures()[handle.index].generation;
        };
        const auto buffer = [&](RGBufferHandle& handle)
        {
            if (handle.index < resources.Buffers().size())
                handle.generation = resources.Buffers()[handle.index].generation;
        };
        for (RGCompiledPass& pass : result.frame.passes)
        {
            for (RGCompiledImageBarrier& barrier : pass.preImageBarriers)
                texture(barrier.texture);
            for (RGCompiledImageBarrier& barrier : pass.postImageBarriers)
                texture(barrier.texture);
            for (RGCompiledBufferBarrier& barrier : pass.preBufferBarriers)
                buffer(barrier.buffer);
            for (RGCompiledBufferBarrier& barrier : pass.postBufferBarriers)
                buffer(barrier.buffer);
        }
        for (RGCompiledResourceLifetime& lifetime : result.frame.lifetimes)
        {
            if (lifetime.isTexture)
                texture(lifetime.texture);
            else
                buffer(lifetime.buffer);
        }
        result.frame.topologyHash = builder.ComputeTopologyHash();
        return result;
    }

    ++m_Misses;
    RGCompileResult result;
    result.errors = builder.Validate();
    RGCompileResult compiled = RenderGraphCompiler::Compile(builder.Passes(), resources, options);
    result.errors.insert(result.errors.end(), compiled.errors.begin(), compiled.errors.end());
    result.frame = std::move(compiled.frame);
    result.frame.topologyHash = builder.ComputeTopologyHash();

    m_Valid = result.Ok();
    if (m_Valid)
    {
        m_Key = std::move(key);
        m_Frame = result.frame;
    }
    return result;
}

} // namespace UHE::RHI
