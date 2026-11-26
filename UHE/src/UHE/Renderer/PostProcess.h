#pragma once

#include <string>
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/Renderer3D/RenderSettings.h"

namespace UHE
{

/// @brief How the tonemap pass maps scene-referred colour to the display.
struct TonemapSettings
{
    float Exposure = 1.0f;       ///< linear multiplier applied before the curve
    float BloomIntensity = 0.0f; ///< share of the bloom input in the result; used only when one is given
    /// Reads the input upside down. The scene renders with Vulkan's y-down clip
    /// space and no flip in the projection; the editor flips when it displays
    /// the viewport image, a game drawing into the backbuffer flips here.
    bool FlipY = false;
    /// FXAA on the tonemapped result, in the same pass (each tap is tonemapped first).
    bool Fxaa = false;
};

/**
 * @brief Post-processing passes, declared into a FrameGraph.
 *
 * Each pass is a frame graph pass: it reads its input by usage and writes its
 * output, and the graph orders it after whatever produced the input and puts
 * both textures in the right state. Pipelines are built per output format on
 * first use and rebuilt when the shader hot-reloads.
 */
class UHE_API PostProcess
{
public:
    /**
     * @brief Declares a pass that tonemaps \p hdr into \p target.
     * @param targetFormat the format \p target was declared with (the pipeline is built for it)
     * @param bloom        optional image from AddBloomPasses, mixed into \p hdr by
     *                     TonemapSettings::BloomIntensity before exposure; an invalid
     *                     handle (the default) tonemaps \p hdr alone
     * @return the version of \p target the pass writes.
     */
    static RHI::FGHandle<RHI::FGTexture> AddTonemapPass(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> hdr,
                                                        RHI::FGHandle<RHI::FGTexture> target,
                                                        RHI::TextureFormat targetFormat,
                                                        const TonemapSettings& settings = {},
                                                        RHI::FGHandle<RHI::FGTexture> bloom = {});

    /**
     * @brief Tonemaps \p hdr into a texture the pass creates (\p targetName, \p targetDesc).
     *
     * For a target that only the frame owns, or that the caller exports.
     * @return the written target.
     */
    static RHI::FGHandle<RHI::FGTexture> AddTonemapPass(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> hdr,
                                                        const std::string& targetName,
                                                        const RHI::FGTexture::Desc& targetDesc,
                                                        const TonemapSettings& settings,
                                                        RHI::FGHandle<RHI::FGTexture> bloom = {});

    /**
     * @brief Declares the bloom chain over \p hdr; returns the bloomed image at half resolution, or an invalid handle
     * when disabled.
     *
     * Physically based bloom (Jimenez, SIGGRAPH 2014): no threshold, the whole
     * image is downsampled through up to six RGBA16F levels ("Bloom.Down<i>",
     * the first at half resolution) and upsampled back, each level its own
     * texture ("Bloom.Up<i>" = Down<i> + tent(Up<i+1>)), so no pass reads what
     * it writes. Pass the result and BloomSettings::Intensity to AddTonemapPass.
     *
     * @param width,height size of \p hdr, in pixels
     * @return an invalid handle when \p settings disables bloom or \p hdr is too
     *         small for even one level.
     */
    static RHI::FGHandle<RHI::FGTexture> AddBloomPasses(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> hdr,
                                                        u32 width, u32 height, const BloomSettings& settings);

    /// @brief Destroys the pipelines and shaders. Called at application shutdown.
    static void Shutdown();
};

} // namespace UHE
