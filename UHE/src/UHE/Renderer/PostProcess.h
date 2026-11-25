#pragma once

#include "UHE/RHI/FrameGraph/FrameGraph.h"

namespace UHE
{

/// @brief How the tonemap pass maps scene-referred colour to the display.
struct TonemapSettings
{
    float Exposure = 1.0f; ///< linear multiplier applied before the curve
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
     * @return the version of \p target the pass writes.
     */
    static RHI::FGHandle<RHI::FGTexture> AddTonemapPass(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> hdr,
                                                        RHI::FGHandle<RHI::FGTexture> target,
                                                        RHI::TextureFormat targetFormat,
                                                        const TonemapSettings& settings = {});

    /// @brief Destroys the pipelines and shaders. Called at application shutdown.
    static void Shutdown();
};

} // namespace UHE
