#include "uhepch.h"
#include "UHE/Physics/Joints3D.h"
#include <cmath>
#include <map>
// Jolt
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include "UHE/Physics/PhysicsSystem3D.h"
#include "UHE/Scene/Components.h"
#include "UHE/Scene/Entity.h"
#include "UHE/Scene/Scene.h"

namespace UHE::Physics
{
namespace
{
constexpr uint32_t kNoBody = 0xFFFFFFFF;

JPH::Vec3 ToJolt(const glm::vec3& v)
{
    return {v.x, v.y, v.z};
}

glm::vec3 ToGlm(JPH::Vec3Arg v)
{
    return {v.GetX(), v.GetY(), v.GetZ()};
}

/// The body of an entity, or the invalid id (the world) when it has none.
JPH::BodyID BodyOf(entt::registry& registry, entt::entity entity)
{
    if (entity == entt::null || !registry.valid(entity))
        return {};
    const auto* rb = registry.try_get<RigidBody3DComponent>(entity);
    return rb != nullptr && rb->RuntimeBodyID != kNoBody ? JPH::BodyID(rb->RuntimeBodyID) : JPH::BodyID();
}

JPH::TwoBodyConstraint* Constraint(const Joint3DComponent& joint)
{
    return static_cast<JPH::TwoBodyConstraint*>(joint.RuntimeConstraint);
}

void Release(Scene& scene, Joint3DComponent& joint)
{
    JPH::TwoBodyConstraint* constraint = Constraint(joint);
    if (constraint == nullptr)
        return;
    if (JPH::PhysicsSystem* world = scene.GetPhysicsSystem3D().GetJoltPhysicsSystem())
        world->RemoveConstraint(constraint);
    constraint->Release();
    joint.RuntimeConstraint = nullptr;
    joint.RuntimeHasPosition = false;
}
} // namespace

void ApplyMotor(Scene& scene, Joint3DComponent& joint)
{
    JPH::TwoBodyConstraint* constraint = Constraint(joint);
    if (constraint == nullptr)
        return;
    const JPH::EMotorState state = joint.Motor == Joint3DComponent::MotorMode::Velocity   ? JPH::EMotorState::Velocity
                                   : joint.Motor == Joint3DComponent::MotorMode::Position ? JPH::EMotorState::Position
                                                                                          : JPH::EMotorState::Off;
    const float limit = std::max(joint.MotorMaxForce, 0.0f);
    if (joint.Type == Joint3DComponent::JointType::Hinge)
    {
        auto* hinge = static_cast<JPH::HingeConstraint*>(constraint);
        JPH::MotorSettings& motor = hinge->GetMotorSettings();
        motor.SetTorqueLimit(limit);
        motor.mSpringSettings.mFrequency = std::max(joint.MotorFrequency, 0.0f);
        motor.mSpringSettings.mDamping = std::max(joint.MotorDamping, 0.0f);
        hinge->SetMotorState(state);
        if (state == JPH::EMotorState::Velocity)
            hinge->SetTargetAngularVelocity(joint.MotorTarget);
        else if (state == JPH::EMotorState::Position)
            hinge->SetTargetAngle(joint.MotorTarget);
    }
    else if (joint.Type == Joint3DComponent::JointType::Slider)
    {
        auto* slider = static_cast<JPH::SliderConstraint*>(constraint);
        JPH::MotorSettings& motor = slider->GetMotorSettings();
        motor.SetForceLimit(limit);
        motor.mSpringSettings.mFrequency = std::max(joint.MotorFrequency, 0.0f);
        motor.mSpringSettings.mDamping = std::max(joint.MotorDamping, 0.0f);
        slider->SetMotorState(state);
        if (state == JPH::EMotorState::Velocity)
            slider->SetTargetVelocity(joint.MotorTarget);
        else if (state == JPH::EMotorState::Position)
            slider->SetTargetPosition(joint.MotorTarget);
    }
    else
    {
        return; // fixed and ball joints have no motor
    }
    // A motor must wake its bodies, or a resting arm ignores its new target.
    if (JPH::BodyInterface* bodies = scene.GetPhysicsSystem3D().GetBodyInterface())
        bodies->ActivateConstraint(constraint);
}

void CreateJoints(Scene& scene)
{
    PhysicsSystem3D& physics = scene.GetPhysicsSystem3D();
    JPH::PhysicsSystem* world = physics.GetJoltPhysicsSystem();
    JPH::BodyInterface* bodies = physics.GetBodyInterface();
    if (world == nullptr || bodies == nullptr)
        return;
    entt::registry& registry = scene.GetRegistry();

    // Jointed bodies do not collide with each other: a hinge between two
    // touching links would otherwise fight its own contact. They share one
    // group, a sub-group each, with the jointed pairs disabled in the table.
    std::map<uint32_t, JPH::CollisionGroup::SubGroupID> subGroups;
    std::vector<std::pair<uint32_t, uint32_t>> pairs;

    auto view = registry.view<Joint3DComponent>();
    for (auto entity : view)
    {
        Joint3DComponent& joint = view.get<Joint3DComponent>(entity);
        Release(scene, joint);
        const JPH::BodyID self = BodyOf(registry, entity);
        if (self.IsInvalid())
        {
            UHE_CORE_WARN("Joint on '{0}': the entity has no 3D rigid body; not built",
                          Entity{entity, &scene}.GetComponent<TagComponent>().Tag);
            continue;
        }
        entt::entity otherEntity = entt::null;
        if (joint.ConnectedEntity != 0)
            otherEntity = static_cast<entt::entity>(scene.GetEntityWithUUID(joint.ConnectedEntity));
        const JPH::BodyID other = BodyOf(registry, otherEntity);
        if (joint.ConnectedEntity != 0 && other.IsInvalid())
        {
            UHE_CORE_WARN("Joint on '{0}': the connected entity has no 3D rigid body; not built",
                          Entity{entity, &scene}.GetComponent<TagComponent>().Tag);
            continue;
        }

        // World-space frame from this body's pose at the start.
        const JPH::RVec3 position = bodies->GetPosition(self);
        const JPH::Quat rotation = bodies->GetRotation(self);
        const JPH::RVec3 anchor = position + rotation * ToJolt(joint.Anchor);
        JPH::Vec3 axis = rotation * ToJolt(joint.Axis);
        axis = axis.LengthSq() > 1e-12f ? axis.Normalized() : JPH::Vec3::sAxisY();
        const JPH::Vec3 normal = axis.GetNormalizedPerpendicular();

        JPH::TwoBodyConstraint* constraint = nullptr;
        switch (joint.Type)
        {
            case Joint3DComponent::JointType::Fixed:
            {
                JPH::FixedConstraintSettings settings;
                settings.mAutoDetectPoint = true; // keep the bodies exactly as placed
                constraint = bodies->CreateConstraint(&settings, other, self);
                break;
            }
            case Joint3DComponent::JointType::Hinge:
            {
                JPH::HingeConstraintSettings settings;
                settings.mPoint1 = settings.mPoint2 = anchor;
                settings.mHingeAxis1 = settings.mHingeAxis2 = axis;
                settings.mNormalAxis1 = settings.mNormalAxis2 = normal;
                if (joint.LimitsEnabled)
                {
                    // Jolt's hinge limits lie in [-pi, 0] and [0, pi].
                    settings.mLimitsMin = std::clamp(joint.LimitMin, -JPH::JPH_PI, 0.0f);
                    settings.mLimitsMax = std::clamp(joint.LimitMax, 0.0f, JPH::JPH_PI);
                }
                settings.mMaxFrictionTorque = std::max(joint.Friction, 0.0f);
                constraint = bodies->CreateConstraint(&settings, other, self);
                break;
            }
            case Joint3DComponent::JointType::Slider:
            {
                JPH::SliderConstraintSettings settings;
                settings.mPoint1 = settings.mPoint2 = anchor;
                settings.mSliderAxis1 = settings.mSliderAxis2 = axis;
                settings.mNormalAxis1 = settings.mNormalAxis2 = normal;
                if (joint.LimitsEnabled)
                {
                    settings.mLimitsMin = std::min(joint.LimitMin, 0.0f);
                    settings.mLimitsMax = std::max(joint.LimitMax, 0.0f);
                }
                settings.mMaxFrictionForce = std::max(joint.Friction, 0.0f);
                constraint = bodies->CreateConstraint(&settings, other, self);
                break;
            }
            case Joint3DComponent::JointType::Ball:
            {
                JPH::PointConstraintSettings settings;
                settings.mPoint1 = settings.mPoint2 = anchor;
                constraint = bodies->CreateConstraint(&settings, other, self);
                break;
            }
        }
        if (constraint == nullptr)
            continue;
        constraint->AddRef(); // the component's reference; Release() drops it
        world->AddConstraint(constraint);
        joint.RuntimeConstraint = constraint;
        joint.RuntimeHasPosition = false;
        ApplyMotor(scene, joint);

        if (!other.IsInvalid())
        {
            for (const JPH::BodyID& id : {self, other})
            {
                const uint32_t key = id.GetIndexAndSequenceNumber();
                if (!subGroups.contains(key))
                    subGroups.emplace(key, static_cast<JPH::CollisionGroup::SubGroupID>(subGroups.size()));
            }
            pairs.emplace_back(self.GetIndexAndSequenceNumber(), other.GetIndexAndSequenceNumber());
        }
    }

    if (!pairs.empty())
    {
        JPH::Ref<JPH::GroupFilterTable> table = new JPH::GroupFilterTable(static_cast<JPH::uint>(subGroups.size()));
        for (const auto& [a, b] : pairs)
            table->DisableCollision(subGroups.at(a), subGroups.at(b));
        for (const auto& [key, subGroup] : subGroups)
            bodies->SetCollisionGroup(JPH::BodyID(key), JPH::CollisionGroup(table, 0, subGroup));
    }
}

void DestroyJointsOf(Scene& scene, Entity entity)
{
    if (!entity)
        return;
    entt::registry& registry = scene.GetRegistry();
    const u64 uuid = entity.GetUUID();
    auto view = registry.view<Joint3DComponent>();
    for (auto other : view)
    {
        Joint3DComponent& joint = view.get<Joint3DComponent>(other);
        if (other == static_cast<entt::entity>(entity) || (uuid != 0 && joint.ConnectedEntity == uuid))
            Release(scene, joint);
    }
}

void DestroyAllJoints(Scene& scene)
{
    auto view = scene.GetRegistry().view<Joint3DComponent>();
    for (auto entity : view)
        Release(scene, view.get<Joint3DComponent>(entity));
}

void UpdateSensors(Scene& scene, float dt)
{
    if (dt <= 0.0f)
        return;
    entt::registry& registry = scene.GetRegistry();

    auto joints = registry.view<Joint3DComponent>();
    for (auto entity : joints)
    {
        Joint3DComponent& joint = joints.get<Joint3DComponent>(entity);
        JPH::TwoBodyConstraint* constraint = Constraint(joint);
        if (constraint == nullptr)
            continue;
        float position = 0.0f;
        bool angular = false;
        if (joint.Type == Joint3DComponent::JointType::Hinge)
        {
            position = static_cast<JPH::HingeConstraint*>(constraint)->GetCurrentAngle();
            angular = true;
        }
        else if (joint.Type == Joint3DComponent::JointType::Slider)
        {
            position = static_cast<JPH::SliderConstraint*>(constraint)->GetCurrentPosition();
        }
        else
        {
            continue;
        }
        if (joint.RuntimeHasPosition)
        {
            float delta = position - joint.RuntimePosition;
            // An unlimited hinge wraps at +-pi: the short way round is the motion.
            if (angular)
                delta = std::remainder(delta, 2.0f * JPH::JPH_PI);
            joint.RuntimeVelocity = delta / dt;
        }
        joint.RuntimePosition = position;
        joint.RuntimeHasPosition = true;
    }

    JPH::BodyInterface* bodies = scene.GetPhysicsSystem3D().GetBodyInterface();
    if (bodies == nullptr)
        return;
    auto rigid = registry.view<RigidBody3DComponent>();
    for (auto entity : rigid)
    {
        RigidBody3DComponent& rb = rigid.get<RigidBody3DComponent>(entity);
        if (rb.RuntimeBodyID == kNoBody)
            continue;
        const glm::vec3 velocity = ToGlm(bodies->GetLinearVelocity(JPH::BodyID(rb.RuntimeBodyID)));
        rb.RuntimeLinearAcceleration =
            rb.RuntimeHasPreviousVelocity ? (velocity - rb.RuntimePreviousVelocity) / dt : glm::vec3(0.0f);
        rb.RuntimePreviousVelocity = velocity;
        rb.RuntimeHasPreviousVelocity = true;
    }
}

std::optional<RayHit> CastRay(Scene& scene, const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                              u32 ignoreEntity)
{
    JPH::PhysicsSystem* world = scene.GetPhysicsSystem3D().GetJoltPhysicsSystem();
    const float length = glm::length(direction);
    if (world == nullptr || length < 1e-9f || maxDistance <= 0.0f)
        return std::nullopt;
    const glm::vec3 dir = direction / length;
    const JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), ToJolt(dir * maxDistance));

    JPH::BodyID ignored;
    if (ignoreEntity != ~0u)
        ignored = BodyOf(scene.GetRegistry(), static_cast<entt::entity>(ignoreEntity));
    const JPH::IgnoreSingleBodyFilter filter(ignored);
    JPH::RayCastResult hit;
    if (!world->GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter))
        return std::nullopt;

    RayHit result;
    result.Distance = hit.mFraction * maxDistance;
    result.Point = origin + dir * result.Distance;
    result.Entity = static_cast<u32>(world->GetBodyInterface().GetUserData(hit.mBodyID));
    JPH::BodyLockRead lock(world->GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded())
        result.Normal =
            ToGlm(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction)));
    return result;
}

} // namespace UHE::Physics
