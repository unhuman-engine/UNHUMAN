#pragma once
#include "UHE/Core/Core.h"
#include <glm/glm.hpp>
#include "entt.hpp"
#include <vector>

namespace UHE::RD3d
{

// One light in the GPU light list, filled from scene components and from
// KHR_lights_punctual lights carried by loaded glTF models.
//
// Spot support (KHR_lights_punctual, issue #29 Tier 3): type 2 is a spot. The
// two cone angles live in the z/w pads of Type_Radius_Pad as COSINES
// (cos(inner), cos(outer)) so the shader needs no trig per fragment.
// Direction_Pad is the spot's forward axis (normalized, pointing from the
// light into the scene). Directional lights keep their direction in
// PositionOrDirection and leave Direction_Pad unused.
//
// Area lights (type 3) are rectangles: z/w of Type_Radius_Pad hold the half
// width and half height, Direction_Pad the side they light (w = 1 when they
// light both sides), and AreaRight_Pad the rectangle's width axis.
struct LightData {
    glm::vec4 Type_Radius_Pad;     // x = type (0 dir, 1 point, 2 spot, 3 area), y = radius, zw = cones / half size
    glm::vec4 PositionOrDirection; // position for point/spot/area, direction for directional
    glm::vec4 Direction_Pad;       // spot/area forward axis; directional: w = 1 when it casts shadows
    glm::vec4 ColorIntensity;
    glm::vec4 AreaRight_Pad{0.0f}; // area: xyz the rectangle's width axis; point/spot/area: w = 1 when it casts shadows
};

class UHE_API LightSystem
{
public:
    static std::vector<LightData> ExtractLights(entt::registry& registry);
};

} // namespace UHE::RD3d
