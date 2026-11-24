#include "UHE/RHI/FrameGraph/TransientTexturePool.h"
#include <algorithm>
#include <utility>

namespace UHE::RHI
{

TransientTexturePool::TransientTexturePool(CreateFn create, DestroyFn destroy, u32 retireAfterFrames)
    : m_Create(std::move(create)), m_Destroy(std::move(destroy)), m_RetireAfter(std::max(retireAfterFrames, 1u))
{
}

bool TransientTexturePool::SameShape(const TextureDesc& a, const TextureDesc& b)
{
    // debugName is a label, not part of the shape.
    return a.width == b.width && a.height == b.height && a.depth == b.depth && a.mipLevels == b.mipLevels &&
           a.format == b.format && static_cast<u32>(a.usage) == static_cast<u32>(b.usage);
}

TextureHandle TransientTexturePool::Acquire(const TextureDesc& desc)
{
    for (Entry& entry : m_Entries)
    {
        if (!entry.InUse && SameShape(entry.Desc, desc))
        {
            entry.InUse = true;
            entry.LastUsedFrame = m_Frame;
            return entry.Texture;
        }
    }

    TextureHandle texture = m_Create ? m_Create(desc) : nullptr;
    if (texture == nullptr)
        return nullptr;
    ++m_Created;
    Entry entry;
    entry.Desc = desc;
    entry.Desc.debugName = nullptr; // the caller's string need not outlive the call
    entry.Texture = texture;
    entry.LastUsedFrame = m_Frame;
    entry.InUse = true;
    m_Entries.push_back(entry);
    return texture;
}

void TransientTexturePool::EndFrame()
{
    for (Entry& entry : m_Entries)
        entry.InUse = false;

    const u64 frame = m_Frame;
    const u32 retireAfter = m_RetireAfter;
    auto retired = std::remove_if(m_Entries.begin(), m_Entries.end(),
                                  [&](const Entry& entry)
                                  {
                                      if (frame - entry.LastUsedFrame < retireAfter)
                                          return false;
                                      if (m_Destroy)
                                          m_Destroy(entry.Texture);
                                      ++m_Destroyed;
                                      return true;
                                  });
    m_Entries.erase(retired, m_Entries.end());
    ++m_Frame;
}

void TransientTexturePool::Clear()
{
    for (const Entry& entry : m_Entries)
    {
        if (m_Destroy)
            m_Destroy(entry.Texture);
        ++m_Destroyed;
    }
    m_Entries.clear();
}

} // namespace UHE::RHI
