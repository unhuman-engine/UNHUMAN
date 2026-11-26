#pragma once

#include <glm/glm.hpp>
#include <optional>
#include "UHE/Core/Core.h"

namespace UHE
{
class Scene;
class Entity;
struct Joint3DComponent;
} // namespace UHE

namespace UHE::Physics
{
// ─── Joints and sensors for robot simulation (issue #69) ────────────────
// Jolt constraints built from Joint3DComponents, and the per-tick sensor
// readings (joint encoders, IMU) scripts read. Everything here runs on the
// simulation tick, so a run is reproducible tick for tick.

/// @brief Builds every Joint3DComponent's constraint. Call after the bodies exist (Scene::OnRuntimeStart).
UHE_API void CreateJoints(Scene& scene);

/// @brief Removes the constraints attached to \p entity, its own and those naming it. Before its body goes.
UHE_API void DestroyJointsOf(Scene& scene, Entity entity);

/// @brief Removes every constraint (Scene::OnRuntimeStop, before the world goes).
UHE_API void DestroyAllJoints(Scene& scene);

/// @brief Reads encoders and IMUs after a physics step of \p dt seconds.
UHE_API void UpdateSensors(Scene& scene, float dt);

/// @brief Applies the component's Motor, MotorTarget and MotorMaxForce to its running constraint.
UHE_API void ApplyMotor(Scene& scene, Joint3DComponent& joint);

/// @brief What a ray from \p origin along \p direction hit first.
struct RayHit
{
    float Distance = 0.0f;
    glm::vec3 Point{0.0f};
    glm::vec3 Normal{0.0f};
    u32 Entity = 0; ///< entt::entity of the body hit, as an integer
};

/**
 * @brief Casts a ray against the running 3D world (a range sensor, one lidar beam).
 * @param ignoreEntity an entt::entity whose body the ray passes through (the sensor's own), or ~0u
 * @return the nearest hit within \p maxDistance, or nothing
 */
UHE_API std::optional<RayHit> CastRay(Scene& scene, const glm::vec3& origin, const glm::vec3& direction,
                                      float maxDistance, u32 ignoreEntity = ~0u);

} // namespace UHE::Physics
