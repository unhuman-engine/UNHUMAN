#pragma once
#include "RHITypes.h"

namespace UHE::RHI
{

/**
 * \brief Read/write image resource (render target, sampled texture, storage image).
 *
 * \note The ImGui id is an opaque backend handle (a Vulkan descriptor set) cast to
 *       void* so the editor can hand it to ImGui::Image without knowing the backend.
 */
class RHITexture
{
public:
    virtual ~RHITexture() = default;

    [[nodiscard]] virtual const TextureDesc& GetDesc() const = 0;
    /// \returns the backend handle ImGui needs to draw this texture, or nullptr.
    virtual void* GetImGuiTextureID() = 0;
    /// \returns the bindless slot for this texture, or 0 when bindless is off.
    [[nodiscard]] virtual u32 GetTextureIndex() const { return 0; }
};

} // namespace UHE::RHI
