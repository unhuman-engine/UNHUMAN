#pragma once

#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/Renderer3D/RenderSettings.h"

namespace UHE
{

/**
 * @brief Screen-space ambient occlusion, declared into a FrameGraph (SSAO.slang).
 *
 * Reads the scene's depth prepass and writes an R8 occlusion image at the
 * same size: 1 where ambient light reaches, toward 0 in creases and at
 * contacts. Basic3D multiplies its ambient term by it; direct light is left
 * alone, which is what separates it from a darkening post effect.
 */
class UHE_API SSAO
{
public:
    /**
     * @brief Declares the occlusion pass and its depth-aware blur.
     * @param depth the prepass depth, read as a texture
     * @param projection the projection the depth was rendered with
     * @return the blurred occlusion image
     */
    static RHI::FGHandle<RHI::FGTexture> AddPasses(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> depth,
                                                   u32 width, u32 height, const glm::mat4& projection,
                                                   const SSAOSettings& settings);

    /// @brief Destroys the pipeline and shaders. Called at application shutdown.
    static void Shutdown();
};

} // namespace UHE
