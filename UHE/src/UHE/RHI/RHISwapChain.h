#pragma once
#include "RHITypes.h"

namespace UHE::RHI
{

/**
 * \brief Presentation surface and image rotation.
 *
 * Owns the chain of presentable images and the acquire/present handshake with
 * the windowing system. The device drives it once per frame: AcquireNextImage()
 * at the start, Present() at the end.
 *
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainKHR.html
 */
class UHE_API RHISwapChain
{
public:
    virtual ~RHISwapChain() = default;

    // ── Frame sync & presentation ────────────────────────────────────────────

    /// Acquires the next presentable image, blocking only as long as the swapchain allows.
    virtual void AcquireNextImage() = 0;
    /// Presents the most recently acquired image and advances the rotation.
    virtual void Present() = 0;

    // ── Window manipulation ──────────────────────────────────────────────────

    /// Rebuilds the swapchain for a new client area. Safe to call while minimized (0×0).
    virtual void ResizeSwapchain(u32 width, u32 height) = 0;

    // ── Lookups ──────────────────────────────────────────────────────────────

    /// \returns the current swapchain image as an RHI texture handle.
    [[nodiscard]] virtual TextureHandle GetSwapchainImage() = 0;
    /// \returns the swapchain's surface format (usually BGRA8_SRGB).
    [[nodiscard]] virtual TextureFormat GetSwapchainFormat() = 0;
};

} // namespace UHE::RHI
