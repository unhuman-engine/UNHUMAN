#pragma once

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI
{

/// @brief Textures are interchangeable when every field but the debug name matches.
[[nodiscard]] inline bool SameShape(const TextureDesc& a, const TextureDesc& b)
{
    return a.width == b.width && a.height == b.height && a.depth == b.depth && a.mipLevels == b.mipLevels &&
           a.format == b.format && static_cast<u32>(a.usage) == static_cast<u32>(b.usage);
}

/// @brief Buffers are interchangeable when size, usage and host visibility match.
[[nodiscard]] inline bool SameShape(const BufferDesc& a, const BufferDesc& b)
{
    return a.size == b.size && a.usage == b.usage && a.hostVisible == b.hostVisible;
}

/**
 * @brief Physical resources for the ones a frame graph creates, reused across frames.
 *
 * A pass that calls FGBuilder::Create gets an object from here, not a fresh
 * allocation every frame. Each frame takes objects by description; two
 * requests for the same description in one frame get two objects. An object
 * nothing asked for in `retireAfterFrames` frames is destroyed, which is how a
 * resize retires the old size: its description is never asked for again.
 *
 * The delay must exceed the frames in flight, so no submitted frame still
 * uses an object when it goes (the device's Destroy* also defers to the
 * slot's fence). Backend-neutral: create and destroy are injected, and
 * tests/framegraph drives the policy with fakes.
 */
template <class Desc, class Handle> class TransientPool
{
public:
    using CreateFn = std::function<Handle(const Desc&)>;
    using DestroyFn = std::function<void(Handle)>;

    TransientPool(CreateFn create, DestroyFn destroy, u32 retireAfterFrames = 3)
        : m_Create(std::move(create)), m_Destroy(std::move(destroy)), m_RetireAfter(std::max(retireAfterFrames, 1u))
    {
    }
    TransientPool(const TransientPool&) = delete;
    TransientPool& operator=(const TransientPool&) = delete;

    /// @brief An object matching \p desc that nothing else holds this frame; nullptr if creation failed.
    Handle Acquire(const Desc& desc)
    {
        for (Entry& entry : m_Entries)
        {
            if (!entry.InUse && SameShape(entry.Shape, desc))
            {
                entry.InUse = true;
                entry.LastUsedFrame = m_Frame;
                return entry.Object;
            }
        }
        Handle object = m_Create ? m_Create(desc) : Handle{};
        if (object == Handle{})
            return Handle{};
        ++m_Created;
        Entry entry;
        entry.Shape = desc;
        entry.Shape.debugName = nullptr; // the caller's string need not outlive the call
        entry.Object = object;
        entry.LastUsedFrame = m_Frame;
        entry.InUse = true;
        m_Entries.push_back(entry);
        return object;
    }

    /// @brief Ends the frame: frees this frame's claims and destroys objects unused for too long.
    void EndFrame()
    {
        for (Entry& entry : m_Entries)
            entry.InUse = false;
        const u64 frame = m_Frame;
        auto retired = std::remove_if(m_Entries.begin(), m_Entries.end(),
                                      [&](const Entry& entry)
                                      {
                                          if (frame - entry.LastUsedFrame < m_RetireAfter)
                                              return false;
                                          if (m_Destroy)
                                              m_Destroy(entry.Object);
                                          ++m_Destroyed;
                                          return true;
                                      });
        m_Entries.erase(retired, m_Entries.end());
        ++m_Frame;
    }

    /// @brief Destroys every object. Call after the device is idle.
    void Clear()
    {
        for (const Entry& entry : m_Entries)
        {
            if (m_Destroy)
                m_Destroy(entry.Object);
            ++m_Destroyed;
        }
        m_Entries.clear();
    }

    [[nodiscard]] size_t Size() const { return m_Entries.size(); }
    [[nodiscard]] u64 CreatedCount() const { return m_Created; }
    [[nodiscard]] u64 DestroyedCount() const { return m_Destroyed; }

private:
    struct Entry
    {
        Desc Shape;
        Handle Object{};
        u64 LastUsedFrame = 0;
        bool InUse = false; ///< claimed this frame
    };

    CreateFn m_Create;
    DestroyFn m_Destroy;
    u32 m_RetireAfter;
    std::vector<Entry> m_Entries;
    u64 m_Frame = 0;
    u64 m_Created = 0;
    u64 m_Destroyed = 0;
};

using TransientTexturePool = TransientPool<TextureDesc, TextureHandle>;
using TransientBufferPool = TransientPool<BufferDesc, BufferHandle>;

} // namespace UHE::RHI
