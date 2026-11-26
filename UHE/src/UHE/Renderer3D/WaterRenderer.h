#pragma once

#include <glm/glm.hpp>
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHITypes.h"

namespace UHE
{
struct WaterComponent;

/**
 * @brief Draws water surfaces (WaterComponent) with Water.slang.
 *
 * Used by SceneRenderer: CopyOpaque fills a new HDR image with the opaque
 * scene, then each Submit draws one surface over it that reads the opaque
 * colour and depth as textures.
 */
class UHE_API WaterRenderer
{
public:
    struct Frame
    {
        glm::mat4 ViewProjection{1.0f};
        glm::mat4 Projection{1.0f}; ///< turns the opaque depth back into distances
        glm::vec3 CameraPosition{0.0f};
        float Time = 0.0f;
        glm::vec2 WindDirection{1.0f, 0.0f}; ///< for surfaces that follow the wind
        i32 SceneColor = -1;                 ///< bindless index of the opaque HDR colour
        i32 SceneDepth = -1;                 ///< bindless index of the opaque depth
    };

    /// @brief Copies the opaque colour (bindless \p sceneColor) into the bound single target.
    static void CopyOpaque(i32 sceneColor);
    /// @brief Selects \p frame for the surfaces that follow (target: HDR colour + entity IDs).
    static void Begin(const Frame& frame);
    /// @brief Draws one surface with world transform \p transform; \p entityID makes it pickable.
    static void Submit(const glm::mat4& transform, const WaterComponent& water, int entityID);
    static void End();

    /// @brief Destroys the pipeline and shaders. Called at application shutdown.
    /// @brief The shader loaded and the device can run it; when false the
    ///        scene's water passes are left out and the opaque image is kept.
    [[nodiscard]] static bool IsAvailable();

    static void Shutdown();
};

} // namespace UHE
