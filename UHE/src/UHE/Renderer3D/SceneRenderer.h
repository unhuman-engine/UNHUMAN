#pragma once

#include <array>
#include <functional>
#include <string>
#include "UHE/Core/Core.h"
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/Renderer3D/RenderSettings.h"

namespace UHE
{
class Scene;

/**
 * @brief Declares the passes that render a 3D scene into a FrameGraph.
 *
 * With lighting on, a frame is: cascaded shadow maps for the first
 * shadow-casting directional light, a depth prepass and SSAO, the colour pass
 * into an RGBA16F target (with the entity IDs beside it), bloom, and the
 * tonemap into the output. Each feature follows the scene's RenderSettings
 * and is left out of the graph when it is off. The debug shading modes
 * (Unlit, Solid, Normals, Wireframe) draw straight into the output instead.
 *
 * Everything that depends on the camera is resolved when the first pass
 * executes, not when the graph is declared: the editor UI that runs in
 * between can still move the camera or an object, and every pass of the frame
 * then agrees on where things are.
 */
class UHE_API SceneRenderer
{
public:
    struct Desc
    {
        u32 Width = 0;
        u32 Height = 0;
        /// Drawn by every pass. Held until the frame's passes have run: the UI
        /// between declaring and executing them can replace the active scene
        /// (Play, Stop, Open), and a raw pointer would outlive it.
        Ref<Scene> SceneData;
        bool Runtime = false; ///< play mode: transforms blend physics ticks (Scene::PrepareRenderFrame)
        bool Lit = true;      ///< false for the debug shading modes: no HDR, shadows, SSAO or bloom
        float Time = 0.0f;    ///< seconds; animates the wind
        std::array<float, 4> ClearColor{0.0f, 0.0f, 0.0f, 1.0f};
        std::string ColorName;  ///< the output colour (RGBA8_SRGB), named so the caller can export it
        std::string EntityName; ///< the entity IDs (R32_SINT)
        /// The camera, asked once when the first pass executes.
        std::function<SceneRenderView()> ResolveView;
        /// Records the colour pass's draws (scene, sprites, icons); viewport and scissor are set.
        std::function<void(RHI::FGContext&)> DrawScene;
        /// Lit view only: editor overlays (the grid), drawn after the water into
        /// the HDR image with the opaque depth bound, so water does not refract them.
        std::function<void(RHI::FGContext&)> DrawOverlays;
    };

    struct Output
    {
        RHI::FGHandle<RHI::FGTexture> Color;  ///< RGBA8_SRGB, display-ready
        RHI::FGHandle<RHI::FGTexture> Entity; ///< R32_SINT, -1 where nothing is
    };

    /// @brief Declares the frame's scene passes into \p graph.
    static Output AddPasses(RHI::FrameGraph& graph, const Desc& desc);

    /// @brief Destroys the pipelines its passes built. Called at application shutdown.
    static void Shutdown();
};

} // namespace UHE
