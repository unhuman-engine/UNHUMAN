#pragma once

#include <functional>
#include <vector>
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI
{

/**
 * @brief Physical textures for the textures a frame graph creates, reused across frames.
 *
 * A pass that calls FGBuilder::Create gets a texture from here, not a fresh
 * allocation every frame. Each frame takes textures by description; two
 * requests for the same description in one frame get two textures. A texture
 * nothing asked for in `retireAfterFrames` frames is destroyed, which is how a
 * resize retires the old size: its description is never asked for again.
 *
 * The delay must exceed the frames in flight, so no submitted frame still
 * reads a texture when it goes (the device's DestroyTexture also defers to the
 * slot's fence). Backend-neutral: create and destroy are injected, and
 * tests/framegraph drives the policy with fakes.
 */
class UHE_API TransientTexturePool
{
public:
    using CreateFn = std::function<TextureHandle(const TextureDesc&)>;
    using DestroyFn = std::function<void(TextureHandle)>;

    TransientTexturePool(CreateFn create, DestroyFn destroy, u32 retireAfterFrames = 3);
    ~TransientTexturePool() = default;
    TransientTexturePool(const TransientTexturePool&) = delete;
    TransientTexturePool& operator=(const TransientTexturePool&) = delete;

    /// @brief A texture matching \p desc that nothing else holds this frame; nullptr if creation failed.
    TextureHandle Acquire(const TextureDesc& desc);
    /// @brief Ends the frame: frees this frame's claims and destroys textures unused for too long.
    void EndFrame();
    /// @brief Destroys every texture. Call after the device is idle.
    void Clear();

    [[nodiscard]] size_t Size() const { return m_Entries.size(); }
    [[nodiscard]] u64 CreatedCount() const { return m_Created; }
    [[nodiscard]] u64 DestroyedCount() const { return m_Destroyed; }

private:
    struct Entry
    {
        TextureDesc Desc;
        TextureHandle Texture = nullptr;
        u64 LastUsedFrame = 0;
        bool InUse = false; ///< claimed this frame
    };

    [[nodiscard]] static bool SameShape(const TextureDesc& a, const TextureDesc& b);

    CreateFn m_Create;
    DestroyFn m_Destroy;
    u32 m_RetireAfter;
    std::vector<Entry> m_Entries;
    u64 m_Frame = 0;
    u64 m_Created = 0;
    u64 m_Destroyed = 0;
};

} // namespace UHE::RHI
