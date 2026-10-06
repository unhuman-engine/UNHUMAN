#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "UHE/Core/Core.h"

namespace UHE::RD3d
{

// Issue #41: analytic two-bone IK (arm/leg chains: shoulder->elbow->hand,
// hip->knee->foot). Works in world space on joint positions and returns the
// world-space rotations that bend the chain toward the target; the caller
// composes them onto the bones' current global rotations.
struct UHE_API TwoBoneIKResult
{
    bool Solved = false;
    // Rotation applied to the root joint (aims the chain at the target).
    glm::quat RootRotation{1.0f, 0.0f, 0.0f, 0.0f};
    // Rotation applied to the mid joint (the elbow/knee bend).
    glm::quat MidRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

class UHE_API IKSolver
{
public:
    // rootPos/midPos/endPos: current world positions of the three joints.
    // targetPos: where the end joint should reach (auto-clamped to the
    //   reachable range so over-extension bends straight instead of flipping).
    // poleVector: a world position the mid joint bends toward (elbow/knee
    //   direction). Collinear with the chain falls back to the current bend.
    static TwoBoneIKResult SolveTwoBone(const glm::vec3& rootPos, const glm::vec3& midPos,
                                        const glm::vec3& endPos, const glm::vec3& targetPos,
                                        const glm::vec3& poleVector);
};

} // namespace UHE::RD3d
