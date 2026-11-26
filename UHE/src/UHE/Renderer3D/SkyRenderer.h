#pragma once

#include <glm/glm.hpp>
#include "UHE/Core/Core.h"
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/Renderer3D/RenderSettings.h"

namespace UHE
{

/**
 * @brief Draws the sky (SkySettings) behind a scene: a fullscreen pass over
 *        the HDR image that fills every pixel the depth buffer left at the
 *        far plane.
 *
 * SceneRenderer declares the pass ("Scene.Sky") after the colour pass and
 * before water, so water refracts the sky like anything else behind it.
 */
class UHE_API SkyRenderer
{
public:
    /// @brief Everything one draw needs that depends on the camera and the lights.
    struct Frame
    {
        glm::mat4 InverseViewProjection{1.0f};
        glm::vec3 CameraPosition{0.0f};
        bool HasSun = false;
        glm::vec3 ToSun{0.0f, 1.0f, 0.0f}; ///< unit vector towards the sun
        glm::vec3 SunColor{1.0f};          ///< the sun light's linear colour x intensity
    };

    /// @brief Whether \p settings draw anything (the Color mode draws nothing: the clear colour is the sky).
    [[nodiscard]] static bool Draws(const SkySettings& settings);

    /**
     * @brief Loads the panorama \p settings name, when it changed since the last call.
     *
     * Call while declaring the frame, not from a pass: the upload submits work
     * of its own. A path that does not load is logged once and the sky falls
     * back to its horizon colour.
     */
    static void PrepareImage(const SkySettings& settings);

    /// @brief Records the fullscreen sky draw; viewport and scissor must be set.
    static void Draw(RHI::FGContext& context, const SkySettings& settings, const Frame& frame, i32 depthTexture);

    /**
     * @brief The procedural sky's linear colour straight up and at the
     *        horizon, at noon, x intensity: for ambient light and for water
     *        to reflect.
     */
    static glm::vec3 ZenithRadiance(const SkySettings& settings);
    static glm::vec3 HorizonRadiance(const SkySettings& settings);

    /// @brief How much daylight the procedural sky has with the sun at
    ///        elevation \p sinElevation (its direction's y): 1 by day, 0 at night,
    ///        the same curve Sky.slang darkens the sky with.
    static float Daylight(float sinElevation);

    static void Shutdown();
};

} // namespace UHE
