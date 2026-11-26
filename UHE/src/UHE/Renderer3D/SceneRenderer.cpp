#include "uhepch.h"
#include "UHE/Renderer3D/SceneRenderer.h"
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include "UHE/RHI/RHICommandBuffer.h"
#include "UHE/RHI/RHITexture.h"
#include "UHE/Renderer/PostProcess.h"
#include "UHE/Renderer/Renderer2D.h"
#include "UHE/Renderer3D/LightSystem.h"
#include "UHE/Renderer3D/Renderer3D.h"
#include "UHE/Renderer3D/SSAO.h"
#include "UHE/Renderer3D/SkyRenderer.h"
#include "UHE/Renderer3D/WaterRenderer.h"
#include "UHE/Scene/Scene.h"

namespace UHE
{

namespace
{
using namespace RHI;

/// Casters this far behind a cascade (toward the light) still cast into it.
constexpr float kShadowCasterReach = 100.0f;
constexpr int kMaxCascades = 4;

/// What every pass of one frame shares; filled when the first of them executes.
struct FrameState
{
    SceneRenderer::Desc Desc;
    RenderSettings Settings;
    bool Shadows = false;
    u32 CascadeResolution = 0;
    u32 AtlasSize = 0;

    bool LocalShadows = false; ///< a local light casts shadows: the local atlas pass exists
    u32 LocalTileResolution = 0;
    u32 LocalTilesPerRow = 1;
    u32 LocalAtlasSize = 0;

    bool Resolved = false;
    SkyRenderer::Frame Sky; ///< the camera and sun the sky pass draws with
    SceneRenderView View;
    Renderer3D::SceneFrameData Frame;
    int CascadeCount = 0;
    std::vector<Renderer3D::ShadowTile> LocalTiles;
};

/// Whether the light list has a point, spot or area light with CastShadows on.
bool HasLocalShadowCaster(const std::vector<RD3d::LightData>& lights)
{
    for (const RD3d::LightData& light : lights)
        if (light.Type_Radius_Pad.x != 0.0f && light.AreaRight_Pad.w > 0.5f)
            return true;
    return false;
}

/**
 * @brief Gives the shadow-casting local lights nearest the camera their tiles of the local atlas.
 *
 * A spot or area light takes one tile, a point light six (one per cube face,
 * a little wider than 90 degrees so faces overlap at their seams). Lights
 * that no longer fit are skipped: they light without shadows this frame.
 */
void FitLocalShadows(FrameState& state, const std::vector<RD3d::LightData>& lights)
{
    struct Candidate
    {
        int Index;
        float Distance;
    };
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < lights.size(); ++i)
    {
        const RD3d::LightData& light = lights[i];
        if (light.Type_Radius_Pad.x == 0.0f || light.AreaRight_Pad.w <= 0.5f)
            continue;
        candidates.push_back(
            {static_cast<int>(i), glm::length(glm::vec3(light.PositionOrDirection) - state.View.Position)});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.Distance < b.Distance; });

    const int maxTiles = std::clamp(state.Settings.LocalShadows.MaxTiles, 1, 64);
    const float tileScale = 1.0f / static_cast<float>(state.LocalTilesPerRow);
    const float resolution = static_cast<float>(state.LocalTileResolution);
    const auto addTile =
        [&](int lightIndex, int face, const glm::vec3& position, const glm::vec3& direction, float fov, float range)
    {
        const u32 slot = static_cast<u32>(state.LocalTiles.size());
        const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        Renderer3D::ShadowTile tile;
        tile.ViewProj =
            glm::perspective(fov, 1.0f, 0.05f, std::max(range, 0.1f)) * glm::lookAt(position, position + direction, up);
        const u32 column = slot % state.LocalTilesPerRow;
        const u32 row = slot / state.LocalTilesPerRow; // whole rows: integer division
        tile.Rect = glm::vec4(static_cast<float>(column) * tileScale, static_cast<float>(row) * tileScale, tileScale,
                              tileScale);
        tile.Params = glm::vec4(static_cast<float>(lightIndex), static_cast<float>(face),
                                2.0f * std::tan(fov * 0.5f) / resolution, 0.0f);
        state.LocalTiles.push_back(tile);
    };

    for (const Candidate& candidate : candidates)
    {
        const RD3d::LightData& light = lights[static_cast<size_t>(candidate.Index)];
        const int type = static_cast<int>(light.Type_Radius_Pad.x);
        const int need = type == 1 ? 6 : 1;
        if (static_cast<int>(state.LocalTiles.size()) + need > maxTiles)
            continue;
        const glm::vec3 position(light.PositionOrDirection);
        const float range = light.Type_Radius_Pad.y;
        if (type == 1)
        {
            static const std::array<glm::vec3, 6> kFaces = {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
                                                            glm::vec3(0, 1, 0), glm::vec3(0, -1, 0),
                                                            glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)};
            for (int face = 0; face < 6; ++face)
                addTile(candidate.Index, face, position, kFaces[face], glm::radians(95.0f), range);
        }
        else
        {
            const glm::vec3 direction = glm::normalize(glm::vec3(light.Direction_Pad));
            // A spot covers its outer cone; an area light the half-space in front of the panel.
            const float fov =
                type == 2 ? std::min(2.0f * std::acos(std::clamp(light.Type_Radius_Pad.w, -1.0f, 1.0f)) + 0.1f, 2.9f)
                          : 2.4f;
            addTile(candidate.Index, -1, position, direction, fov, range);
        }
    }
}

float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

glm::vec3 SrgbToLinear(const glm::vec3& c)
{
    return {SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b)};
}

i32 BindlessIndex(TextureHandle texture)
{
    return texture != nullptr ? static_cast<i32>(reinterpret_cast<RHITexture*>(texture)->GetTextureIndex()) : -1;
}

/// The first directional light that casts shadows, or -1.
int FindShadowLight(const std::vector<RD3d::LightData>& lights)
{
    for (size_t i = 0; i < lights.size(); ++i)
    {
        if (lights[i].Type_Radius_Pad.x == 0.0f && lights[i].Direction_Pad.w > 0.5f)
            return static_cast<int>(i);
    }
    return -1;
}

/**
 * @brief Fits one shadow cascade around each slice of the view frustum.
 *
 * Slices split the view distance between uniform and logarithmic steps
 * (SplitLambda). Each cascade is an orthographic box around the slice's
 * bounding sphere: a sphere does not change size as the camera turns, so the
 * texel size stays constant, and the box is snapped to whole texels, so
 * shadow edges do not crawl while the camera moves.
 */
void FitCascades(FrameState& state, const glm::vec3& lightDirection)
{
    const ShadowSettings& shadows = state.Settings.Shadows;
    const glm::mat4& projection = state.View.Projection;
    const glm::mat4 inverseViewProjection = glm::inverse(projection * state.View.View);
    const glm::mat4 inverseProjection = glm::inverse(projection);

    // View distance of an NDC depth, and the reverse, from the projection itself.
    const auto viewDistance = [&](float ndcZ)
    {
        const glm::vec4 p = inverseProjection * glm::vec4(0.0f, 0.0f, ndcZ, 1.0f);
        return -p.z / p.w;
    };
    const auto ndcDepth = [&](float distance)
    {
        const glm::vec4 clip = projection * glm::vec4(0.0f, 0.0f, -distance, 1.0f);
        return clip.z / clip.w;
    };

    const float nearDistance = std::max(viewDistance(0.0f), 0.01f);
    const float farDistance = std::max(std::min(viewDistance(1.0f), shadows.Distance), nearDistance + 0.1f);
    const int count = std::clamp(shadows.CascadeCount, 1, kMaxCascades);
    const float lambda = std::clamp(shadows.SplitLambda, 0.0f, 1.0f);
    const float resolution = static_cast<float>(state.CascadeResolution);

    const glm::vec3 direction = glm::normalize(lightDirection);
    const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

    Renderer3D::SceneFrameData& frame = state.Frame;
    float sliceStart = nearDistance;
    for (int i = 0; i < count; ++i)
    {
        const float p = static_cast<float>(i + 1) / static_cast<float>(count);
        const float logarithmic = nearDistance * std::pow(farDistance / nearDistance, p);
        const float uniform = nearDistance + (farDistance - nearDistance) * p;
        const float sliceEnd = glm::mix(uniform, logarithmic, lambda);

        std::array<glm::vec3, 8> corners{};
        glm::vec3 centre(0.0f);
        int c = 0;
        for (const float z : {ndcDepth(sliceStart), ndcDepth(sliceEnd)})
        {
            for (const float y : {-1.0f, 1.0f})
            {
                for (const float x : {-1.0f, 1.0f})
                {
                    const glm::vec4 world = inverseViewProjection * glm::vec4(x, y, z, 1.0f);
                    corners[c] = glm::vec3(world) / world.w;
                    centre += corners[c];
                    ++c;
                }
            }
        }
        centre /= 8.0f;
        float radius = 0.0f;
        for (const glm::vec3& corner : corners)
            radius = std::max(radius, glm::length(corner - centre));
        radius = std::ceil(radius * 16.0f) / 16.0f;

        const glm::mat4 lightView = glm::lookAt(centre - direction * (radius + kShadowCasterReach), centre, up);
        glm::mat4 lightProjection =
            glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + kShadowCasterReach);

        // Snap the world origin to a texel: the box moves in whole texels.
        const glm::vec4 origin = lightProjection * lightView * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec2 originTexels = glm::vec2(origin) * (resolution * 0.5f);
        const glm::vec2 offset = (glm::round(originTexels) - originTexels) * (2.0f / resolution);
        lightProjection[3][0] += offset.x;
        lightProjection[3][1] += offset.y;

        frame.CascadeViewProj[i] = lightProjection * lightView;
        frame.CascadeSplits[i] = sliceEnd;
        frame.CascadeTexel[i] = 2.0f * radius / resolution;
        // 2x2 atlas: cascade i sits in column i % 2, row i / 2.
        const int column = i % 2;
        const int row = i / 2;
        frame.CascadeRects[i] =
            count == 1 ? glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)
                       : glm::vec4(static_cast<float>(column) * 0.5f, static_cast<float>(row) * 0.5f, 0.5f, 0.5f);
        sliceStart = sliceEnd;
    }
    state.CascadeCount = count;
}

/// @brief Fills the frame's camera, light and setting dependent state; once per frame.
void Resolve(FrameState& state)
{
    if (state.Resolved)
        return;
    state.Resolved = true;

    Scene* scene = state.Desc.SceneData.get();
    if (scene != nullptr)
        scene->PrepareRenderFrame(state.Desc.Runtime);
    if (state.Desc.ResolveView)
        state.View = state.Desc.ResolveView();

    const RenderSettings& settings = state.Settings;
    Renderer3D::SceneFrameData& frame = state.Frame;
    const EnvironmentSettings& environment = settings.Environment;
    frame.AmbientSky = glm::vec4(SrgbToLinear(environment.SkyColor) * environment.AmbientIntensity, 0.0f);
    frame.AmbientGround = glm::vec4(SrgbToLinear(environment.GroundColor) * environment.AmbientIntensity, 0.0f);
    frame.CameraForward =
        glm::vec4(-glm::vec3(state.View.View[0][2], state.View.View[1][2], state.View.View[2][2]), 0.0f);
    frame.Screen = glm::vec4(1.0f / static_cast<float>(state.Desc.Width), 1.0f / static_cast<float>(state.Desc.Height),
                             -1.0f, 1.0f);

    const WindSettings wind = scene != nullptr ? scene->GetEffectiveWind() : settings.Wind;
    const glm::vec2 windDirection =
        glm::length(wind.Direction) > 1e-4f ? glm::normalize(wind.Direction) : glm::vec2(1.0f, 0.0f);
    frame.Wind =
        glm::vec4(windDirection * std::max(wind.Strength, 0.0f), std::max(wind.GustStrength, 0.0f), state.Desc.Time);
    frame.Flags = glm::vec4(1.0f, std::max(wind.GustFrequency, 0.0f), 0.0f, 0.0f);

    frame.Shadow = glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f);
    frame.Sky = glm::vec4(SrgbToLinear(environment.SkyColor), -1.0f);

    // The procedural sky lights the scene and colours the water's reflection
    // like it colours the background, when asked to.
    const SkySettings& sky = settings.Sky;
    if (sky.Mode == SkySettings::SkyMode::Procedural)
    {
        const glm::vec3 zenith = SkyRenderer::ZenithRadiance(sky);
        const glm::vec3 horizon = SkyRenderer::HorizonRadiance(sky);
        frame.Sky = glm::vec4(glm::mix(horizon, zenith, 0.35f), -1.0f);
        if (sky.AmbientFromSky)
        {
            frame.AmbientSky = glm::vec4(glm::mix(horizon, zenith, 0.5f) * environment.AmbientIntensity, 0.0f);
            frame.AmbientGround = glm::vec4(SrgbToLinear(sky.GroundColor) * environment.AmbientIntensity, 0.0f);
        }
    }
    state.Sky.InverseViewProjection = glm::inverse(state.View.Projection * state.View.View);
    state.Sky.CameraPosition = state.View.Position;
    if (scene == nullptr)
        return;
    const std::vector<RD3d::LightData> lights = RD3d::LightSystem::ExtractLights(scene->GetRegistry());
    for (size_t i = 0; i < lights.size(); ++i)
    {
        if (lights[i].Type_Radius_Pad.x == 0.0f)
        {
            frame.Sky.w = static_cast<float>(i); // the sun the water reflects
            const glm::vec3 direction(lights[i].PositionOrDirection);
            state.Sky.HasSun = glm::dot(direction, direction) > 1e-8f;
            state.Sky.ToSun = state.Sky.HasSun ? -glm::normalize(direction) : glm::vec3(0.0f, 1.0f, 0.0f);
            state.Sky.SunColor = glm::vec3(lights[i].ColorIntensity) * lights[i].ColorIntensity.w;
            // Ambient taken from the sky fades with it at night.
            if (sky.Mode == SkySettings::SkyMode::Procedural && sky.AmbientFromSky)
            {
                const float daylight = std::max(SkyRenderer::Daylight(state.Sky.ToSun.y), 0.03f);
                frame.AmbientSky *= daylight;
                frame.AmbientGround *= daylight;
            }
            break;
        }
    }
    if (state.LocalShadows)
    {
        FitLocalShadows(state, lights);
        const int tileBuffer = Renderer3D::SetShadowTiles(state.LocalTiles);
        frame.LocalShadow =
            glm::vec4(-1.0f, static_cast<float>(state.LocalTiles.size()), static_cast<float>(tileBuffer), 0.0f);
        frame.LocalShadowBias =
            glm::vec4(state.Settings.LocalShadows.DepthBias, state.Settings.LocalShadows.NormalBias, 0.0f, 0.0f);
    }
    if (!state.Shadows)
        return;
    const int shadowLight = FindShadowLight(lights);
    if (shadowLight < 0)
        return;
    FitCascades(state, glm::vec3(lights[static_cast<size_t>(shadowLight)].PositionOrDirection));
    frame.Shadow = glm::vec4(-1.0f, static_cast<float>(shadowLight), 1.0f / static_cast<float>(state.AtlasSize),
                             static_cast<float>(state.CascadeCount));
    const ShadowSettings& shadows = settings.Shadows;
    frame.ShadowBias =
        glm::vec4(shadows.DepthBias, shadows.NormalBias, shadows.Softness, shadows.ShowCascades ? 1.0f : 0.0f);
}

} // namespace

SceneRenderer::Output SceneRenderer::AddPasses(FrameGraph& graph, const Desc& desc)
{
    Output output;
    if (desc.Width == 0 || desc.Height == 0)
        return output;

    auto state = std::make_shared<FrameState>();
    state->Desc = desc;
    if (desc.SceneData != nullptr)
        state->Settings = desc.SceneData->GetRenderSettings();
    const RenderSettings& settings = state->Settings;
    const bool lit = desc.Lit;
    const u32 width = desc.Width;
    const u32 height = desc.Height;

    // Whether there is a light to cast shadows is known now; where it points
    // and where the camera is are resolved when the passes run.
    if (lit && settings.Shadows.Enabled && desc.SceneData != nullptr)
    {
        const auto lights = RD3d::LightSystem::ExtractLights(desc.SceneData->GetRegistry());
        state->Shadows = FindShadowLight(lights) >= 0;
    }
    if (lit && settings.LocalShadows.Enabled && desc.SceneData != nullptr)
    {
        state->LocalShadows = HasLocalShadowCaster(RD3d::LightSystem::ExtractLights(desc.SceneData->GetRegistry()));
        state->LocalTileResolution = static_cast<u32>(std::clamp(settings.LocalShadows.TileResolution, 256, 2048));
        const u32 tiles = static_cast<u32>(std::clamp(settings.LocalShadows.MaxTiles, 1, 64));
        state->LocalTilesPerRow = static_cast<u32>(std::ceil(std::sqrt(static_cast<float>(tiles))));
        // The atlas stays within 8192 texels per side; tiles shrink to fit.
        state->LocalTileResolution = std::min(state->LocalTileResolution, 8192u / state->LocalTilesPerRow);
        state->LocalAtlasSize = state->LocalTileResolution * state->LocalTilesPerRow;
    }
    state->CascadeResolution = static_cast<u32>(std::clamp(settings.Shadows.Resolution, 256, 4096));
    state->AtlasSize = settings.Shadows.CascadeCount > 1 ? state->CascadeResolution * 2 : state->CascadeResolution;

    // ── Shadow cascades: one D32 atlas, a 2x2 grid of cascades ──
    FGHandle<FGTexture> shadowAtlas;
    if (state->Shadows)
    {
        struct ShadowData
        {
            FGHandle<FGTexture> Atlas;
        };
        const ShadowData& shadow = graph.AddPass<ShadowData>(
            "Scene.Shadows",
            [&](FGBuilder& builder, ShadowData& d)
            {
                d.Atlas = builder.Write(
                    builder.Create("Scene.ShadowAtlas", {state->AtlasSize, state->AtlasSize, TextureFormat::D32_FLOAT}),
                    FGWrite::DepthTarget);
            },
            [state](const ShadowData&, FGContext& context)
            {
                Resolve(*state);
                Scene* scene = state->Desc.SceneData.get();
                if (scene == nullptr || state->CascadeCount == 0)
                    return;
                Renderer3D::SetFrameData(state->Frame);
                RHICommandBuffer& cmd = context.Cmd();
                const float size = static_cast<float>(state->AtlasSize);
                for (int i = 0; i < state->CascadeCount; ++i)
                {
                    const glm::vec4 rect = state->Frame.CascadeRects[i];
                    const u32 x = static_cast<u32>(rect.x * size);
                    const u32 y = static_cast<u32>(rect.y * size);
                    const u32 extent = static_cast<u32>(rect.z * size);
                    cmd.SetViewport(static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(extent),
                                    static_cast<f32>(extent));
                    cmd.SetScissor(static_cast<i32>(x), static_cast<i32>(y), extent, extent);
                    Renderer3D::BeginDepthPass(state->Frame.CascadeViewProj[i], TextureFormat::D32_FLOAT);
                    scene->RenderDepth();
                    Renderer3D::EndDepthPass();
                }
            });
        shadowAtlas = shadow.Atlas;
    }

    // ── Local shadows: point, spot and area lights, one atlas of tiles ──
    FGHandle<FGTexture> localAtlas;
    if (state->LocalShadows)
    {
        struct LocalData
        {
            FGHandle<FGTexture> Atlas;
        };
        const LocalData& local = graph.AddPass<LocalData>(
            "Scene.LocalShadows",
            [&](FGBuilder& builder, LocalData& d)
            {
                d.Atlas = builder.Write(
                    builder.Create("Scene.LocalShadowAtlas",
                                   {state->LocalAtlasSize, state->LocalAtlasSize, TextureFormat::D32_FLOAT}),
                    FGWrite::DepthTarget);
            },
            [state](const LocalData&, FGContext& context)
            {
                Resolve(*state);
                Scene* scene = state->Desc.SceneData.get();
                if (scene == nullptr || state->LocalTiles.empty())
                    return;
                Renderer3D::SetFrameData(state->Frame);
                RHICommandBuffer& cmd = context.Cmd();
                const float size = static_cast<float>(state->LocalAtlasSize);
                for (const Renderer3D::ShadowTile& tile : state->LocalTiles)
                {
                    const u32 x = static_cast<u32>(tile.Rect.x * size);
                    const u32 y = static_cast<u32>(tile.Rect.y * size);
                    const u32 extent = state->LocalTileResolution;
                    cmd.SetViewport(static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(extent),
                                    static_cast<f32>(extent));
                    cmd.SetScissor(static_cast<i32>(x), static_cast<i32>(y), extent, extent);
                    Renderer3D::BeginDepthPass(tile.ViewProj, TextureFormat::D32_FLOAT);
                    scene->RenderDepth();
                    Renderer3D::EndDepthPass();
                }
            });
        localAtlas = local.Atlas;
    }

    // ── Depth prepass and SSAO ──
    FGHandle<FGTexture> prepassDepth;
    FGHandle<FGTexture> occlusion;
    if (lit && settings.SSAO.Enabled && desc.SceneData != nullptr)
    {
        struct PrepassData
        {
            FGHandle<FGTexture> Depth;
        };
        const PrepassData& prepass = graph.AddPass<PrepassData>(
            "Scene.DepthPrepass",
            [&](FGBuilder& builder, PrepassData& d)
            {
                d.Depth = builder.Write(builder.Create("Scene.Depth", {width, height, TextureFormat::D24_UNORM_S8}),
                                        FGWrite::DepthTarget);
            },
            [state](const PrepassData&, FGContext& context)
            {
                Resolve(*state);
                Renderer3D::SetFrameData(state->Frame);
                RHICommandBuffer& cmd = context.Cmd();
                cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
                cmd.SetScissor(0, 0, context.Width(), context.Height());
                Renderer3D::BeginDepthPass(state->View.Projection * state->View.View, TextureFormat::D24_UNORM_S8);
                state->Desc.SceneData->RenderDepth();
                Renderer3D::EndDepthPass();
            });
        prepassDepth = prepass.Depth;
        // The projection is the one the prepass will use only if the camera
        // does not change before it runs; occlusion is a soft effect, and a
        // one-frame lag in a zoom is not visible in it.
        const glm::mat4 projection = desc.ResolveView ? desc.ResolveView().Projection : glm::mat4(1.0f);
        occlusion = SSAO::AddPasses(graph, prepassDepth, width, height, projection, settings.SSAO);
    }

    // ── Colour pass ──
    struct SceneData
    {
        FGHandle<FGTexture> Color;
        FGHandle<FGTexture> Entity;
        FGHandle<FGTexture> Depth;
        FGHandle<FGTexture> Shadow;
        FGHandle<FGTexture> LocalShadow;
        FGHandle<FGTexture> Occlusion;
    };
    const SceneData& scene = graph.AddPass<SceneData>(
        lit ? "Scene.Color" : "Scene.Debug",
        [&](FGBuilder& builder, SceneData& d)
        {
            FGAttachment background;
            background.ClearColor = desc.ClearColor;
            const TextureFormat colorFormat = lit ? TextureFormat::RGBA16F : TextureFormat::RGBA8_SRGB;
            const std::string colorName = lit ? std::string("Scene.HDR") : desc.ColorName;
            d.Color = builder.Write(builder.Create(colorName, {width, height, colorFormat}), FGWrite::RenderTarget,
                                    background);
            FGAttachment noEntity;
            noEntity.ClearColor = {-1.0f, -1.0f, -1.0f, -1.0f};
            d.Entity = builder.Write(builder.Create(desc.EntityName, {width, height, TextureFormat::R32_SINT}),
                                     FGWrite::RenderTarget, noEntity);
            if (prepassDepth.IsValid())
            {
                FGAttachment keep;
                keep.Load = LoadOp::Load;
                d.Depth = builder.Write(prepassDepth, FGWrite::DepthTarget, keep);
            }
            else
            {
                d.Depth = builder.Write(builder.Create("Scene.Depth", {width, height, TextureFormat::D24_UNORM_S8}),
                                        FGWrite::DepthTarget);
            }
            if (shadowAtlas.IsValid())
                d.Shadow = builder.Read(shadowAtlas);
            if (localAtlas.IsValid())
                d.LocalShadow = builder.Read(localAtlas);
            if (occlusion.IsValid())
                d.Occlusion = builder.Read(occlusion);
        },
        [state, lit](const SceneData& d, FGContext& context)
        {
            Resolve(*state);
            if (lit)
            {
                Renderer3D::SceneFrameData frame = state->Frame;
                if (d.Shadow.IsValid())
                    frame.Shadow.x = static_cast<float>(BindlessIndex(context.Get(d.Shadow)));
                if (d.Occlusion.IsValid())
                    frame.Screen.z = static_cast<float>(BindlessIndex(context.Get(d.Occlusion)));
                if (d.LocalShadow.IsValid())
                    frame.LocalShadow.x = static_cast<float>(BindlessIndex(context.Get(d.LocalShadow)));
                Renderer3D::SetFrameData(frame);
                Renderer3D::SetEntityTargetFormat(TextureFormat::RGBA16F);
                Renderer2D::SetColorFormat(TextureFormat::RGBA16F);
            }

            RHICommandBuffer& cmd = context.Cmd();
            cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
            cmd.SetScissor(0, 0, context.Width(), context.Height());
            if (state->Desc.DrawScene)
                state->Desc.DrawScene(context);

            // Nothing drawn after this pass inherits the HDR target or the frame data.
            Renderer3D::ClearFrameData();
            Renderer3D::SetEntityTargetFormat(TextureFormat::RGBA8_SRGB);
            Renderer2D::SetColorFormat(TextureFormat::RGBA8_SRGB);
        });
    output.Entity = scene.Entity;
    if (!lit)
    {
        output.Color = scene.Color;
        return output;
    }

    FGHandle<FGTexture> hdr = scene.Color;
    FGHandle<FGTexture> entity = scene.Entity;

    // ── Sky: behind everything, where the colour pass left the far plane ──
    if (SkyRenderer::Draws(settings.Sky))
    {
        SkyRenderer::PrepareImage(settings.Sky); // uploads, so not from inside a pass
        struct SkyData
        {
            FGHandle<FGTexture> Color;
            FGHandle<FGTexture> Depth;
        };
        const SkyData& sky = graph.AddPass<SkyData>(
            "Scene.Sky",
            [&](FGBuilder& builder, SkyData& d)
            {
                FGAttachment keep;
                keep.Load = LoadOp::Load;
                d.Color = builder.Write(hdr, FGWrite::RenderTarget, keep);
                d.Depth = builder.Read(scene.Depth);
            },
            [state](const SkyData& d, FGContext& context)
            {
                Resolve(*state);
                RHICommandBuffer& cmd = context.Cmd();
                cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
                cmd.SetScissor(0, 0, context.Width(), context.Height());
                SkyRenderer::Draw(context, state->Settings.Sky, state->Sky, BindlessIndex(context.Get(d.Depth)));
            });
        hdr = sky.Color;
    }

    // ── Water: the opaque scene copied into a new HDR image, surfaces over it ──
    if (desc.SceneData != nullptr && desc.SceneData->HasWater())
    {
        // The surfaces sample the opaque colour, so they cannot draw into it:
        // a copy first, then the water over the copy.
        struct CopyData
        {
            FGHandle<FGTexture> Opaque;
            FGHandle<FGTexture> Output;
        };
        const CopyData& copy = graph.AddPass<CopyData>(
            "Scene.WaterCopy",
            [&](FGBuilder& builder, CopyData& d)
            {
                d.Opaque = builder.Read(hdr);
                // Every pixel is written by the copy.
                d.Output = builder.Write(builder.Create("Scene.HDR.Water", {width, height, TextureFormat::RGBA16F}),
                                         FGWrite::RenderTarget);
            },
            [](const CopyData& d, FGContext& context)
            {
                RHICommandBuffer& cmd = context.Cmd();
                cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
                cmd.SetScissor(0, 0, context.Width(), context.Height());
                WaterRenderer::CopyOpaque(BindlessIndex(context.Get(d.Opaque)));
            });

        struct WaterData
        {
            FGHandle<FGTexture> Opaque;
            FGHandle<FGTexture> Depth;
            FGHandle<FGTexture> Output;
            FGHandle<FGTexture> Entity;
        };
        const WaterData& water = graph.AddPass<WaterData>(
            "Scene.Water",
            [&](FGBuilder& builder, WaterData& d)
            {
                d.Opaque = builder.Read(hdr);
                d.Depth = builder.Read(scene.Depth);
                FGAttachment keep;
                keep.Load = LoadOp::Load;
                d.Output = builder.Write(copy.Output, FGWrite::RenderTarget, keep);
                // Surfaces write their entity, so clicking the water selects it.
                d.Entity = builder.Write(scene.Entity, FGWrite::RenderTarget, keep);
            },
            [state](const WaterData& d, FGContext& context)
            {
                Resolve(*state);
                RHICommandBuffer& cmd = context.Cmd();
                cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
                cmd.SetScissor(0, 0, context.Width(), context.Height());

                WaterRenderer::Frame frame;
                frame.ViewProjection = state->View.Projection * state->View.View;
                frame.Projection = state->View.Projection;
                frame.CameraPosition = state->View.Position;
                frame.Time = state->Desc.Time;
                const glm::vec2 wind(state->Frame.Wind.x, state->Frame.Wind.y);
                frame.WindDirection = glm::length(wind) > 1e-4f ? glm::normalize(wind) : glm::vec2(0.0f);
                frame.SceneColor = BindlessIndex(context.Get(d.Opaque));
                frame.SceneDepth = BindlessIndex(context.Get(d.Depth));
                WaterRenderer::Begin(frame);
                state->Desc.SceneData->RenderWater();
                WaterRenderer::End();
            });
        hdr = water.Output;
        entity = water.Entity;
    }

    // ── Editor overlays (the grid): after the water, tested against the opaque depth ──
    if (desc.DrawOverlays)
    {
        struct OverlayData
        {
            FGHandle<FGTexture> Color;
            FGHandle<FGTexture> Entity;
            FGHandle<FGTexture> Depth;
        };
        const OverlayData& overlays = graph.AddPass<OverlayData>(
            "Scene.Overlays",
            [&](FGBuilder& builder, OverlayData& d)
            {
                FGAttachment keep;
                keep.Load = LoadOp::Load;
                d.Color = builder.Write(hdr, FGWrite::RenderTarget, keep);
                d.Entity = builder.Write(entity, FGWrite::RenderTarget, keep);
                d.Depth = builder.Write(scene.Depth, FGWrite::DepthTarget, keep);
            },
            [state](const OverlayData&, FGContext& context)
            {
                RHICommandBuffer& cmd = context.Cmd();
                cmd.SetViewport(0.0f, 0.0f, static_cast<f32>(context.Width()), static_cast<f32>(context.Height()));
                cmd.SetScissor(0, 0, context.Width(), context.Height());
                Renderer3D::SetEntityTargetFormat(TextureFormat::RGBA16F);
                state->Desc.DrawOverlays(context);
                Renderer3D::SetEntityTargetFormat(TextureFormat::RGBA8_SRGB);
            });
        hdr = overlays.Color;
        entity = overlays.Entity;
    }
    output.Entity = entity;

    // ── Bloom and tonemap ──
    const FGHandle<FGTexture> bloom = PostProcess::AddBloomPasses(graph, hdr, width, height, settings.Bloom);
    TonemapSettings tonemap;
    tonemap.Exposure = std::max(settings.Environment.Exposure, 0.0f);
    tonemap.BloomIntensity = std::clamp(settings.Bloom.Intensity, 0.0f, 1.0f);

    tonemap.FlipY = desc.Target.IsValid() && desc.FlipTarget;
    if (desc.Target.IsValid())
        output.Color = PostProcess::AddTonemapPass(graph, hdr, desc.Target, desc.TargetFormat, tonemap, bloom);
    else
        output.Color = PostProcess::AddTonemapPass(graph, hdr, desc.ColorName,
                                                   {width, height, TextureFormat::RGBA8_SRGB}, tonemap, bloom);
    return output;
}

void SceneRenderer::Shutdown()
{
    SSAO::Shutdown();
    SkyRenderer::Shutdown();
    WaterRenderer::Shutdown();
}

} // namespace UHE
