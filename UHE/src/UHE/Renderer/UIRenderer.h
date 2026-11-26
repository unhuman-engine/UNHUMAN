#pragma once

#include <vector>
#include "UHE/Core/Core.h"
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/UI/UISystem.h"

namespace UHE
{

/**
 * @brief Draws a game UI draw list (UI::Update) over a finished frame.
 *
 * One pass, "UI", that loads \p target and alpha-blends every box and line
 * of text onto it in order. Textures and fonts the list names are loaded when
 * the pass is declared, not while it records.
 */
class UHE_API UIRenderer
{
public:
    /**
     * @param target  the image to draw on (the backbuffer, or the editor's viewport colour)
     * @param format  its format (an sRGB one: UI colours are authored in sRGB)
     * @param flipY   the target is displayed upside down (the editor's viewport image)
     * @return the target after the UI, or \p target unchanged when there is nothing to draw
     */
    static RHI::FGHandle<RHI::FGTexture> AddPass(RHI::FrameGraph& graph, RHI::FGHandle<RHI::FGTexture> target,
                                                 RHI::TextureFormat format, u32 width, u32 height, bool flipY,
                                                 const std::vector<UI::DrawCommand>& commands);

    static void Shutdown();
};

} // namespace UHE
