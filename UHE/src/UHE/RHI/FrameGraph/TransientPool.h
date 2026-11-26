#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "UHE/RHI/FrameGraph/FrameGraphTypes.h"
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
 * Within a frame, an object is shared by requests whose lifetimes do not
 * overlap: Acquire takes the [first, last] pass interval the resource is
 * used in, and an object already claimed for disjoint intervals serves it
 * too (memory aliasing between same-shape resources). The backend's first
 * barrier for each resource discards the contents and waits on everything
 * before it, which is what makes the reuse safe. Without an interval a
 * request conflicts with every other one, as before.
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

    static constexpr u32 kWholeFrame = ~0u;

    /**
     * @brief An object matching \p desc that nothing else uses during [\p first, \p last] this frame.
     * @return nullptr if creation failed.
     */
    Handle Acquire(const Desc& desc, u32 first = 0, u32 last = kWholeFrame)
    {
        if (first > last)
            std::swap(first, last);
        for (Entry& entry : m_Entries)
        {
            if (!SameShape(entry.Shape, desc))
                continue;
            const bool free =
                std::none_of(entry.Claims.begin(), entry.Claims.end(), [&](const std::pair<u32, u32>& claim)
                             { return first <= claim.second && claim.first <= last; });
            if (free)
            {
                if (!entry.Claims.empty())
                    ++m_Aliased;
                entry.Claims.emplace_back(first, last);
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
        entry.Claims.emplace_back(first, last);
        m_Entries.push_back(entry);
        return object;
    }

    /// @brief Ends the frame: frees this frame's claims and destroys objects unused for too long.
    void EndFrame()
    {
        for (Entry& entry : m_Entries)
            entry.Claims.clear();
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
    /// @brief Requests served by an object another request already used earlier in the same frame.
    [[nodiscard]] u64 AliasedCount() const { return m_Aliased; }

private:
    struct Entry
    {
        Desc Shape;
        Handle Object{};
        u64 LastUsedFrame = 0;
        std::vector<std::pair<u32, u32>> Claims; ///< pass intervals it serves this frame
    };

    CreateFn m_Create;
    DestroyFn m_Destroy;
    u32 m_RetireAfter;
    std::vector<Entry> m_Entries;
    u64 m_Frame = 0;
    u64 m_Created = 0;
    u64 m_Destroyed = 0;
    u64 m_Aliased = 0;
};

using TransientTexturePool = TransientPool<TextureDesc, TextureHandle>;
using TransientBufferPool = TransientPool<BufferDesc, BufferHandle>;

/// @brief An existing texture serves \p wanted when the shape matches and it supports every usage asked for.
[[nodiscard]] inline bool Covers(const TextureDesc& existing, const TextureDesc& wanted)
{
    const u32 have = static_cast<u32>(existing.usage);
    const u32 need = static_cast<u32>(wanted.usage);
    return existing.width == wanted.width && existing.height == wanted.height && existing.depth == wanted.depth &&
           existing.mipLevels == wanted.mipLevels && existing.format == wanted.format && (have & need) == need;
}

/**
 * @brief The textures frames export (FrameGraph::Export), one object per name, kept across frames.
 *
 * A name keeps its object while the shape stays the same, so the UI holds
 * one texture ID and picking reads the same image every frame. A new size or
 * format (a viewport resize) replaces the object; the old one is destroyed
 * only `retireAfterFrames` later, because frames in flight still read it. A
 * name nothing acquires for that long is released the same way, which is
 * what happens when the layer that exported it is detached.
 *
 * The pool also remembers the state each object was left in, so the next
 * frame's barrier starts from it instead of from Undefined: a new object is
 * Undefined until the backend reports that a frame which wrote it ran
 * (SetState). Backend-neutral like TransientPool; tests/framegraph drives it
 * with fakes.
 */
class ExportPool
{
public:
    using CreateFn = std::function<TextureHandle(const TextureDesc&)>;
    using DestroyFn = std::function<void(TextureHandle)>;

    struct Slot
    {
        TextureHandle Object = nullptr;
        FGState State = FGState::Undefined; ///< what the last frame that ran left it in
    };

    ExportPool(CreateFn create, DestroyFn destroy, u32 retireAfterFrames = 3)
        : m_Create(std::move(create)), m_Destroy(std::move(destroy)), m_RetireAfter(std::max(retireAfterFrames, 1u))
    {
    }
    ExportPool(const ExportPool&) = delete;
    ExportPool& operator=(const ExportPool&) = delete;

    /**
     * @brief The object for \p name, replaced when it cannot serve \p desc.
     * @return the slot, or nullptr when a needed object could not be created (the old one is kept).
     */
    const Slot* Acquire(const std::string& name, const TextureDesc& desc)
    {
        Entry* entry = FindEntry(name);
        if (entry != nullptr && Covers(entry->Shape, desc))
        {
            entry->LastUsedFrame = m_Frame;
            return &entry->Current;
        }

        TextureDesc shape = desc;
        shape.debugName = nullptr; // the caller's string need not outlive the call
        // Same shape, more usage: keep every usage either asked for, so two
        // users of one export do not replace it back and forth.
        if (entry != nullptr && SameSize(entry->Shape, desc))
            shape.usage = entry->Shape.usage | desc.usage;
        TextureDesc createDesc = shape;
        createDesc.debugName = desc.debugName;
        TextureHandle object = m_Create ? m_Create(createDesc) : nullptr;
        if (object == nullptr)
            return nullptr;
        ++m_Created;

        if (entry != nullptr)
        {
            m_Retired.push_back({entry->Current.Object, m_Frame});
            entry->Shape = shape;
            entry->Current = {object, FGState::Undefined};
            entry->LastUsedFrame = m_Frame;
            return &entry->Current;
        }
        m_Entries.push_back({name, shape, {object, FGState::Undefined}, m_Frame});
        return &m_Entries.back().Current;
    }

    /// @brief The current object for \p name, whatever its shape; nullptr if there is none.
    [[nodiscard]] const Slot* Find(const std::string& name) const
    {
        for (const Entry& entry : m_Entries)
            if (entry.Name == name)
                return &entry.Current;
        return nullptr;
    }

    /// @brief Records the state a frame that ran left \p name's object in.
    void SetState(const std::string& name, FGState state)
    {
        if (Entry* entry = FindEntry(name))
            entry->Current.State = state;
    }

    /// @brief Ends the frame: destroys replaced objects and names unused for retireAfterFrames.
    void EndFrame()
    {
        const u64 frame = m_Frame;
        auto due = [&](u64 since) { return frame - since >= m_RetireAfter; };
        auto retired = std::remove_if(m_Retired.begin(), m_Retired.end(),
                                      [&](const Retired& old)
                                      {
                                          if (!due(old.Frame))
                                              return false;
                                          Destroy(old.Object);
                                          return true;
                                      });
        m_Retired.erase(retired, m_Retired.end());
        auto unused = std::remove_if(m_Entries.begin(), m_Entries.end(),
                                     [&](const Entry& entry)
                                     {
                                         if (!due(entry.LastUsedFrame))
                                             return false;
                                         Destroy(entry.Current.Object);
                                         return true;
                                     });
        m_Entries.erase(unused, m_Entries.end());
        ++m_Frame;
    }

    /// @brief Destroys every object. Call after the device is idle.
    void Clear()
    {
        for (const Retired& old : m_Retired)
            Destroy(old.Object);
        for (const Entry& entry : m_Entries)
            Destroy(entry.Current.Object);
        m_Retired.clear();
        m_Entries.clear();
    }

    [[nodiscard]] size_t Size() const { return m_Entries.size(); }
    [[nodiscard]] size_t RetiredSize() const { return m_Retired.size(); }
    [[nodiscard]] u64 CreatedCount() const { return m_Created; }
    [[nodiscard]] u64 DestroyedCount() const { return m_Destroyed; }

private:
    struct Entry
    {
        std::string Name;
        TextureDesc Shape;
        Slot Current;
        u64 LastUsedFrame = 0;
    };
    struct Retired
    {
        TextureHandle Object = nullptr;
        u64 Frame = 0; ///< when it was replaced
    };

    [[nodiscard]] static bool SameSize(const TextureDesc& a, const TextureDesc& b)
    {
        return a.width == b.width && a.height == b.height && a.depth == b.depth && a.mipLevels == b.mipLevels &&
               a.format == b.format;
    }
    Entry* FindEntry(const std::string& name)
    {
        for (Entry& entry : m_Entries)
            if (entry.Name == name)
                return &entry;
        return nullptr;
    }
    void Destroy(TextureHandle object)
    {
        if (m_Destroy)
            m_Destroy(object);
        ++m_Destroyed;
    }

    CreateFn m_Create;
    DestroyFn m_Destroy;
    u32 m_RetireAfter;
    // A returned Slot* is valid until the next Acquire: push_back may move entries.
    std::vector<Entry> m_Entries;
    std::vector<Retired> m_Retired;
    u64 m_Frame = 0;
    u64 m_Created = 0;
    u64 m_Destroyed = 0;
};

} // namespace UHE::RHI
