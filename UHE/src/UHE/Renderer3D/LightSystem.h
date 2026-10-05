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
struct LightData {
    glm::vec4 Type_Radius_Pad;     // x = type (0: Dir, 1: Point, 2: Spot), y = radius, z = cos(inner), w = cos(outer)
    glm::vec4 PositionOrDirection; // position for point/spot, direction for directional
    glm::vec4 Direction_Pad;       // spot forward axis; unused otherwise
    glm::vec4 ColorIntensity;
};

class UHE_API LightSystem
{
public:
    static std::vector<LightData> ExtractLights(entt::registry& registry);
};

} // namespace UHE::RD3d
