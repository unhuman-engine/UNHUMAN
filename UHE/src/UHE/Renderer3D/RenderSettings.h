#pragma once

#include <glm/glm.hpp>
#include <string>

namespace UHE
{

/// @brief Where a scene is rendered from: one camera's matrices for one frame.
struct SceneRenderView
{
    glm::mat4 View{1.0f};
    glm::mat4 Projection{1.0f};
    glm::vec3 Position{0.0f};
};

/// @brief Light that does not come from a light component: a sky and ground hemisphere, and the camera exposure.
struct EnvironmentSettings
{
    glm::vec3 SkyColor{0.60f, 0.70f, 0.85f};    ///< ambient arriving from above (sRGB, as a colour picker shows it)
    glm::vec3 GroundColor{0.30f, 0.27f, 0.24f}; ///< ambient bounced up from below
    float AmbientIntensity = 0.30f;             ///< multiplies both hemisphere colours
    float Exposure = 1.0f;                      ///< linear scale applied before the tonemap curve
};

/// @brief What is drawn behind everything, where no geometry is.
struct SkySettings
{
    enum class SkyMode : int
    {
        Color = 0,      ///< the flat Environment sky colour (the clear colour), as before skies existed
        Procedural = 1, ///< a gradient that follows the scene's sun: horizon haze, sun disk, sunsets, night
        Image = 2,      ///< an equirectangular panorama (.hdr for real HDR light, or .png/.jpg)
    };

    SkyMode Mode = SkyMode::Procedural;
    float Intensity = 1.0f; ///< multiplies the whole sky (both modes)

    // Procedural. Colours are sRGB, as a colour picker shows them.
    glm::vec3 ZenithColor{0.22f, 0.42f, 0.78f};  ///< straight up, in daylight
    glm::vec3 HorizonColor{0.68f, 0.78f, 0.90f}; ///< at the horizon, in daylight
    glm::vec3 GroundColor{0.32f, 0.30f, 0.27f};  ///< below the horizon
    float SunSize = 1.5f;                        ///< apparent diameter of the sun disk, degrees
    float SunBrightness = 30.0f;                 ///< disk radiance as a multiple of the sun light's own colour

    // Image.
    std::string ImagePath;      ///< asset path of an equirectangular image
    float ImageRotation = 0.0f; ///< degrees about the vertical axis

    /// Ambient (the Environment's hemisphere light) follows the sky's own
    /// zenith and ground colours instead of the Environment's sky and ground colours.
    bool AmbientFromSky = false;
};

/// @brief Cascaded shadow maps for the first directional light that casts shadows.
struct ShadowSettings
{
    bool Enabled = true;
    int CascadeCount = 4;      ///< 1..4; each cascade covers a slice of the view distance
    float Distance = 80.0f;    ///< view distance the last cascade reaches; beyond it nothing is shadowed
    float SplitLambda = 0.75f; ///< 0 = uniform splits, 1 = logarithmic (more resolution near the camera)
    int Resolution = 2048;     ///< texels per side of one cascade (the atlas is 2x2 cascades)
    float DepthBias = 0.5f;    ///< constant bias, in shadow-map texels
    float NormalBias = 1.5f;   ///< receiver offset along its normal, in shadow-map texels
    float Softness = 1.0f;     ///< PCF filter radius, in shadow-map texels
    bool ShowCascades = false; ///< tints each cascade (debug view)
};

/// @brief Shadows of point, spot and area lights that have CastShadows on.
struct LocalShadowSettings
{
    bool Enabled = true;
    int TileResolution = 1024; ///< texels per side of one tile; a point light takes six
    int MaxTiles = 16;         ///< tiles in the atlas; the lights nearest the camera get them first
    float DepthBias = 1.0f;    ///< toward the light, in shadow texels
    float NormalBias = 1.5f;   ///< along the surface normal, in shadow texels
};

/// @brief Screen-space ambient occlusion: darkens creases and contact points in the ambient light.
struct SSAOSettings
{
    bool Enabled = true;
    float Radius = 0.5f;    ///< world-space sample radius
    float Intensity = 1.0f; ///< strength of the darkening, 0..4
    float Bias = 0.025f;    ///< depth bias against self-occlusion on flat surfaces
};

/// @brief Physically based bloom: a blurred copy of the whole HDR image mixed back in, with no threshold.
struct BloomSettings
{
    bool Enabled = true;
    float Intensity = 0.04f;   ///< share of the blurred image in the result
    float FilterRadius = 1.0f; ///< upsample tent radius, in texels of the level being upsampled
};

/// @brief The scene's wind: bends vegetation (FoliageComponent) and drives the water's waves.
struct WindSettings
{
    glm::vec2 Direction{1.0f, 0.3f}; ///< direction on the ground plane (x, z); normalized when used
    float Strength = 0.5f;           ///< steady wind, 0..2
    float GustStrength = 0.4f;       ///< extra wind that comes and goes, 0..2
    float GustFrequency = 0.25f;     ///< gusts per second
};

/**
 * @brief How a scene is rendered: saved with the scene (the "Environment" block of a .unhuman).
 *
 * Like Godot's WorldEnvironment. A renderer that does not implement a feature
 * ignores its settings.
 */
struct RenderSettings
{
    EnvironmentSettings Environment;
    SkySettings Sky;
    ShadowSettings Shadows;
    LocalShadowSettings LocalShadows;
    SSAOSettings SSAO;
    BloomSettings Bloom;
    WindSettings Wind;
};

} // namespace UHE
