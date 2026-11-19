#include "uhepch.h"
#include "PhysicsSystem3D.h"
#include "UHE/Core/Log.h"

// Jolt includes
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <cstdarg>
// clang-format on
// Disable common warnings triggered by Jolt
#ifdef JPH_COMPILER_MSVC
    #pragma warning(disable : 4244)
#endif

namespace UHE
{
namespace Physics
{

// Layer definitions
namespace Layers
{
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING = 1;
static constexpr JPH::ObjectLayer NUM_LAYERS = 2;
} // namespace Layers

namespace BroadPhaseLayers
{
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr uint32_t NUM_LAYERS(2);
} // namespace BroadPhaseLayers

// Boilerplate Jolt interfaces
class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl()
    {
        mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }

    uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
    {
        UHE_CORE_ASSERT(inLayer < Layers::NUM_LAYERS, "Invalid layer");
        return mObjectToBroadPhase[inLayer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
    {
        switch (static_cast<JPH::BroadPhaseLayer::Type>(inLayer))
        {
            case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::NON_MOVING):
                return "NON_MOVING";
            case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::MOVING):
                return "MOVING";
            default:
                UHE_CORE_ASSERT(false, "Invalid broad phase layer");
                return "INVALID";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
    {
        switch (inLayer1)
        {
            case Layers::NON_MOVING:
                return inLayer2 == BroadPhaseLayers::MOVING;
            case Layers::MOVING:
                return true;
            default:
                UHE_CORE_ASSERT(false, "Invalid layer");
                return false;
        }
    }
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override
    {
        switch (inObject1)
        {
            case Layers::NON_MOVING:
                return inObject2 == Layers::MOVING;
            case Layers::MOVING:
                return true;
            default:
                UHE_CORE_ASSERT(false, "Invalid layer");
                return false;
        }
    }
};

static void TraceImpl(const char* inFMT, ...)
{
    va_list list;
    va_start(list, inFMT);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), inFMT, list);
    va_end(list);
    UHE_CORE_INFO("JoltPhysics: {0}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
static bool AssertFailedImpl(const char* inExpression, const char* inMessage, const char* inFile, uint32_t inLine)
{
    UHE_CORE_ERROR("JoltPhysics Assert: {0}:{1}: ({2}) {3}", inFile, inLine, inExpression,
                   (inMessage != nullptr ? inMessage : ""));
    return true; // Trigger breakpoint
}
#endif

// -- Engine System --

void PhysicsSystem3D::Init()
{
    JPH::RegisterDefaultAllocator();

    JPH::Trace = TraceImpl;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedImpl;)

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    UHE_CORE_INFO("JoltPhysics initialized successfully.");
}

void PhysicsSystem3D::Shutdown()
{
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

PhysicsSystem3D::~PhysicsSystem3D()
{
    ShutdownScene();
}

void PhysicsSystem3D::InitializeScene()
{
    // A second play session on the same scene must not leak the first world.
    ShutdownScene();

    m_TempAllocator = new JPH::TempAllocatorImpl(10 * 1024 * 1024);
    // hardware_concurrency() may be 0 ("unknown"); 0 - 1 wrapped to Jolt's
    // "auto" by accident. Ask for one worker less than the cores, at least one.
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    m_JobSystem = new JPH::JobSystemThreadPool(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                               std::max(1, cores - 1));

    m_BPLayerInterface = new BPLayerInterfaceImpl();
    m_BroadPhaseLayerFilter = new ObjectVsBroadPhaseLayerFilterImpl();
    m_ObjectLayerPairFilter = new ObjectLayerPairFilterImpl();

    const uint32_t cMaxBodies = 10240;
    const uint32_t cNumBodyMutexes = 0;
    const uint32_t cMaxBodyPairs = 10240;
    const uint32_t cMaxContactConstraints = 10240;

    m_PhysicsSystem = new JPH::PhysicsSystem();
    m_PhysicsSystem->Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
                          *static_cast<BPLayerInterfaceImpl*>(m_BPLayerInterface),
                          *static_cast<ObjectVsBroadPhaseLayerFilterImpl*>(m_BroadPhaseLayerFilter),
                          *static_cast<ObjectLayerPairFilterImpl*>(m_ObjectLayerPairFilter));
}

void PhysicsSystem3D::ShutdownScene()
{
    delete m_PhysicsSystem;
    m_PhysicsSystem = nullptr;

    delete static_cast<ObjectLayerPairFilterImpl*>(m_ObjectLayerPairFilter);
    delete static_cast<ObjectVsBroadPhaseLayerFilterImpl*>(m_BroadPhaseLayerFilter);
    delete static_cast<BPLayerInterfaceImpl*>(m_BPLayerInterface);
    m_ObjectLayerPairFilter = nullptr;
    m_BroadPhaseLayerFilter = nullptr;
    m_BPLayerInterface = nullptr;

    delete m_JobSystem;
    m_JobSystem = nullptr;
    delete m_TempAllocator;
    m_TempAllocator = nullptr;
}

void PhysicsSystem3D::Update(Timestep ts)
{
    if (m_PhysicsSystem == nullptr)
    {
        return;
    }
    const int cCollisionSteps = 1;
    m_PhysicsSystem->Update(ts, cCollisionSteps, m_TempAllocator, m_JobSystem);
}

JPH::BodyInterface* PhysicsSystem3D::GetBodyInterface()
{
    return m_PhysicsSystem != nullptr ? &m_PhysicsSystem->GetBodyInterface() : nullptr;
}

glm::vec3 PhysicsSystem3D::JoltToGlm(const float* joltVec3)
{
    return glm::vec3(joltVec3[0], joltVec3[1], joltVec3[2]);
}

glm::quat PhysicsSystem3D::JoltToGlmQuat(const float* joltQuat)
{
    return glm::quat(joltQuat[3], joltQuat[0], joltQuat[1], joltQuat[2]); // JPH::Quat stores x,y,z,w
}

} // namespace Physics
} // namespace UHE
