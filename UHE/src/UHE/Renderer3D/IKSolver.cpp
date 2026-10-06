#include "IKSolver.h"
#include <cfloat>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

namespace UHE::RD3d {

TwoBoneIKResult IKSolver::SolveTwoBone(const glm::vec3& rootPos, const glm::vec3& midPos,
                                       const glm::vec3& endPos, const glm::vec3& targetPos,
                                       const glm::vec3& poleVector)
{
    TwoBoneIKResult result;

    const glm::vec3 rootToMid = midPos - rootPos;
    const glm::vec3 midToEnd = endPos - midPos;
    const float upperLength = glm::length(rootToMid);
    const float lowerLength = glm::length(midToEnd);
    if (upperLength < FLT_EPSILON || lowerLength < FLT_EPSILON)
        return result;

    const glm::vec3 rootToEnd = endPos - rootPos;
    const glm::vec3 rootToTarget = targetPos - rootPos;
    const float targetDistance = glm::length(rootToTarget);
    if (targetDistance < FLT_EPSILON)
        return result;

    // Clamp inside the reachable annulus: |l1 - l2| < d < l1 + l2.
    const float maxReach = (upperLength + lowerLength) * (1.0f - 1e-4f);
    const float minReach = std::abs(upperLength - lowerLength) * (1.0f + 1e-4f) + FLT_EPSILON;
    const float reach = glm::clamp(targetDistance, minReach, maxReach);
    const glm::vec3 targetDir = rootToTarget / targetDistance;

    // Interior angles of the root-mid-end triangle (law of cosines).
    float rootAngle = std::acos(glm::clamp(
        (upperLength * upperLength + reach * reach - lowerLength * lowerLength) /
            (2.0f * upperLength * reach),
        -1.0f, 1.0f));
    float midAngle = std::acos(glm::clamp(
        (upperLength * upperLength + lowerLength * lowerLength - reach * reach) /
            (2.0f * upperLength * lowerLength),
        -1.0f, 1.0f));

    // Bend plane: contains the chain and the pole. Fall back to the current
    // bend direction when the pole is collinear with the chain.
    glm::vec3 poleDir = poleVector - rootPos;
    glm::vec3 bendNormal = glm::cross(targetDir, poleDir);
    if (glm::length(bendNormal) < 1e-6f)
    {
        glm::vec3 currentBend = glm::cross(rootToEnd, rootToMid);
        bendNormal = glm::length(currentBend) > 1e-6f ? currentBend : glm::vec3(0.0f, 0.0f, 1.0f);
        if (glm::abs(glm::dot(glm::normalize(bendNormal), targetDir)) > 0.999f)
            bendNormal = glm::vec3(0.0f, 1.0f, 0.0f);
        if (glm::abs(glm::dot(glm::normalize(bendNormal), targetDir)) > 0.999f)
            bendNormal = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    bendNormal = glm::normalize(bendNormal);
    // Direction in the bend plane, perpendicular to the target direction.
    glm::vec3 bendDir = glm::cross(bendNormal, targetDir);

    // New joint placement: mid joint off the root->target axis by rootAngle.
    const glm::vec3 newMid = rootPos + upperLength *
        (std::cos(rootAngle) * targetDir + std::sin(rootAngle) * bendDir);
    const glm::vec3 newEnd = rootPos + reach * targetDir;

    // World-space rotations taking the current bone directions to the solved ones.
    glm::quat midRotation = glm::rotation(glm::normalize(midToEnd), glm::normalize(newEnd - newMid));

    glm::vec3 newRootToMid = glm::normalize(newMid - rootPos);
    glm::vec3 currentRootToMid = glm::normalize(rootToMid);
    if (glm::dot(currentRootToMid, newRootToMid) > -0.9999f)
        result.RootRotation = glm::rotation(currentRootToMid, newRootToMid);
    else
        result.RootRotation = glm::angleAxis(glm::pi<float>(), bendNormal);

    // The root rotation above only steers the upper bone; the mid rotation
    // must be expressed relative to the rotated upper bone.
    result.MidRotation = glm::inverse(result.RootRotation) * midRotation;
    result.Solved = true;
    return result;
}

} // namespace UHE::RD3d
