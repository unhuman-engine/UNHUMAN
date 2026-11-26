#pragma once

#include <entt.hpp>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE
{
class Scene;

/**
 * @brief Game UI: layout, the mouse, and what to draw (the UI components in Scene/Components.h).
 *
 * CPU only; UIRenderer draws the list it builds. Pixels, origin at the top
 * left of the screen (the game window, or the editor's viewport), y down.
 */
namespace UI
{
/// @brief The mouse this frame, in UI pixels.
struct Pointer
{
    glm::vec2 Position{-1.0f};
    bool Down = false;     ///< the left button is held
    bool Pressed = false;  ///< it went down this frame
    bool Released = false; ///< it went up this frame
    bool Active = false;   ///< the pointer is over the UI's screen and the game has input (Play)
};

/// @brief One thing to draw, in draw order (later ones on top).
struct DrawCommand
{
    enum class Kind
    {
        Box,  ///< a rounded rect: colour, optional texture, border
        Text, ///< a line (or lines) of text inside Min..Max
    };

    Kind Type = Kind::Box;
    glm::vec2 Min{0.0f};
    glm::vec2 Max{0.0f};
    glm::vec4 Color{1.0f}; ///< sRGB with alpha, as authored
    float CornerRadius = 0.0f;
    float BorderWidth = 0.0f;
    glm::vec4 BorderColor{0.0f};
    std::string TexturePath; ///< Box: an image in the assets, empty for none
    std::string Text;
    float FontSize = 16.0f;
    int Align = 1; ///< 0 left, 1 centre, 2 right
    std::string FontPath;
    entt::entity Entity = entt::null;
};

/// @brief What the mouse did to the UI this frame.
struct Events
{
    std::vector<entt::entity> Clicked; ///< buttons clicked, in the order it happened
    bool PointerOverUI = false;        ///< the pointer is over a visible element (games ignore that click)
};

/**
 * @brief Lays out every UI element for a screen of \p screenSize pixels,
 *        applies \p pointer to buttons and sliders, and returns the draw list.
 *
 * A button is clicked when the mouse goes down and back up on it; a slider
 * follows the mouse from the press until the release, even outside it. Only
 * the topmost interactable element under the mouse reacts.
 *
 * \p scale is screen pixels per UI pixel (Scale): rects, the pointer and
 * UIRectComponent's computed bounds are in UI pixels, the draw list in screen
 * pixels.
 */
UHE_API std::vector<DrawCommand> Update(Scene& scene, glm::vec2 screenSize, const Pointer& pointer,
                                        Events* events = nullptr, float scale = 1.0f);

/**
 * @brief The UI scale for a screen: screen pixels per UI pixel.
 *
 * The UI is designed on a 16:9 screen \p referenceHeight pixels tall; the
 * scale fits that reference inside the screen, so on a wide screen the height
 * decides and on a narrow or portrait one the width does.
 */
[[nodiscard]] UHE_API float Scale(bool scaleWithScreen, float referenceHeight, glm::vec2 screenSize);
} // namespace UI
} // namespace UHE
