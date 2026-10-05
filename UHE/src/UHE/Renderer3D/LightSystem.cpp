#include "uhepch.h"
#include "LightSystem.h"
#include <glm/gtx/quaternion.hpp>
#include "UHE/Scene/Components.h"

namespace UHE::RD3d
{

namespace
{

// glTF lights (KHR_lights_punctual) and scene light components share one GPU
// layout. glTF intensity units differ per type (lux for directional, candela
// for point/spot); the engine's convention is a plain scalar, so the values
// pass through and the shader's PI boost applies uniformly.
LightData MakeDirectional(const glm::vec3& direction, const glm::vec3& color, float intensity)
{
    LightData data;
    data.Type_Radius_Pad = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    data.PositionOrDirection = glm::vec4(direction, 0.0f);
    data.Direction_Pad = glm::vec4(0.0f);
    data.ColorIntensity = glm::vec4(color, intensity);
    return data;
}

LightData MakePoint(const glm::vec3& position, const glm::vec3& color, float intensity, float radius)
{
    LightData data;
    data.Type_Radius_Pad = glm::vec4(1.0f, radius, 0.0f, 0.0f);
    data.PositionOrDirection = glm::vec4(position, 1.0f);
    data.Direction_Pad = glm::vec4(0.0f);
    data.ColorIntensity = glm::vec4(color, intensity);
    return data;
}

LightData MakeSpot(const glm::vec3& position, const glm::vec3& direction, const glm::vec3& color, float intensity,
                   float radius, float innerAngle, float outerAngle)
{
    LightData data;
    // Cone angles are stored as cosines so the shader's inner test is a dot
    // product; clamping keeps a degenerate inner >= outer cone from inverting.
    data.Type_Radius_Pad = glm::vec4(2.0f, radius, std::cos(innerAngle), std::cos(outerAngle));
    data.PositionOrDirection = glm::vec4(position, 1.0f);
    data.Direction_Pad = glm::vec4(glm::normalize(direction), 0.0f);
    data.ColorIntensity = glm::vec4(color, intensity);
    return data;
}

} // namespace

std::vector<LightData> LightSystem::ExtractLights(entt::registry& registry)
{
    std::vector<LightData> lights;

    // Default fallback if no lights exist
    if (registry.view<DirectionalLightComponent>().empty() && registry.view<PointLightComponent>().empty() &&
        registry.view<SpotLightComponent>().empty() && registry.view<Model3DComponent>().empty())
    {
        LightData fallback;
        fallback.Type_Radius_Pad = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f); // Type = 0 (Directional)
        fallback.PositionOrDirection = glm::vec4(0.5f, -1.0f, 0.3f, 0.0f);
        fallback.Direction_Pad = glm::vec4(0.0f);
        fallback.ColorIntensity = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        lights.push_back(fallback);
        return lights;
    }

    auto dirLightView = registry.view<TransformComponent, DirectionalLightComponent>();
    for (auto entity : dirLightView)
    {
        auto [transform, light] = dirLightView.get<TransformComponent, DirectionalLightComponent>(entity);

        auto q = glm::quat(transform.Rotation);

        glm::vec3 direction = glm::normalize(q * glm::vec3(0.0f, 0.0f, -1.0f));

        lights.push_back(MakeDirectional(direction, light.Color, light.Intensity));
    }

    auto pointLightView = registry.view<TransformComponent, PointLightComponent>();
    for (auto entity : pointLightView)
    {
        auto [transform, light] = pointLightView.get<TransformComponent, PointLightComponent>(entity);

        lights.push_back(MakePoint(transform.Translation, light.Color, light.Intensity, light.Radius));
    }

    auto spotLightView = registry.view<TransformComponent, SpotLightComponent>();
    for (auto entity : spotLightView)
    {
        auto [transform, light] = spotLightView.get<TransformComponent, SpotLightComponent>(entity);

        auto q = glm::quat(transform.Rotation);
        glm::vec3 direction = q * glm::vec3(0.0f, 0.0f, -1.0f);

        lights.push_back(MakeSpot(transform.Translation, direction, light.Color, light.Intensity, light.Radius,
                                  light.InnerConeAngle, light.OuterConeAngle));
    }

    // KHR_lights_punctual (issue #29 Tier 3): lights defined INSIDE a glTF
    // asset, placed by the nodes that reference them. They are synthesized
    // here rather than turned into scene entities so they work whether the
    // model is collapsed or expanded. Node placement comes from the loader's
    // accumulated ModelNode::WorldTransform - a node moved AFTER expansion
    // keeps its authored light position until the asset reloads.
    auto modelView = registry.view<TransformComponent, Model3DComponent>();
    for (auto entity : modelView)
    {
        auto [transform, mc] = modelView.get<TransformComponent, Model3DComponent>(entity);
        if (!mc.IsLoaded || !mc.ModelData)
            continue;

        const auto& punctual = mc.ModelData->GetPunctualLights();
        if (punctual.empty())
            continue;

        for (const auto& node : mc.ModelData->GetNodes())
        {
            if (node.LightIndex < 0 || node.LightIndex >= static_cast<int>(punctual.size()))
                continue;

            const auto& light = punctual[static_cast<size_t>(node.LightIndex)];

            // Model entity placement x node placement. glTF measures light
            // direction along the node's local -Z, the same convention the
            // directional light component uses above.
            const glm::vec3 position = glm::vec3(transform.GetTransform() * node.WorldTransform[3]);
            const glm::vec3 direction =
                glm::normalize(glm::mat3(transform.GetTransform()) * glm::vec3(node.WorldTransform[2]) * -1.0f);

            switch (light.type)
            {
                case PunctualLight::Type::Directional:
                    lights.push_back(MakeDirectional(direction, light.Color, light.Intensity));
                    break;
                case PunctualLight::Type::Spot:
                {
                    const float range = std::isfinite(light.Range) ? light.Range : 25.0f;
                    lights.push_back(MakeSpot(position, direction, light.Color, light.Intensity, range,
                                              light.InnerConeAngle, light.OuterConeAngle));
                    break;
                }
                case PunctualLight::Type::Point:
                default:
                {
                    const float range = std::isfinite(light.Range) ? light.Range : 25.0f;
                    lights.push_back(MakePoint(position, light.Color, light.Intensity, range));
                    break;
                }
            }
        }
    }

    return lights;
}

} // namespace UHE::RD3d
