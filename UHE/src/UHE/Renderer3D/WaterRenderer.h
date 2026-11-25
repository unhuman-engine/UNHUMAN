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
 * Used by SceneRenderer's water pass, which writes a new HDR image: Begin
 * copies the opaque scene into it, then each Submit draws one surface that
 * reads the opaque colour and depth as textures.
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

    /// @brief Copies the opaque scene into the bound target and selects \p frame for the surfaces that follow.
    static void Begin(const Frame& frame);
    /// @brief Draws one surface with world transform \p transform.
    static void Submit(const glm::mat4& transform, const WaterComponent& water);
    static void End();

    /// @brief Destroys the pipeline and shaders. Called at application shutdown.
    static void Shutdown();
};

} // namespace UHE
