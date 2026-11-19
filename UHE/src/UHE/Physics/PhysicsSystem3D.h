#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "UHE/Core/Core.h"
#include "UHE/Core/Timestep.h"

// Forward declarations to avoid including Jolt headers everywhere
namespace JPH
{
class PhysicsSystem;
class JobSystemThreadPool;
class TempAllocatorImpl;
class BodyInterface;
} // namespace JPH

namespace UHE::Physics
{

class UHE_API PhysicsSystem3D
{
public:
    PhysicsSystem3D() = default;
    // Releases the scene's world if it is still running: a scene destroyed in
    // play mode (closed, or replaced by another) never reaches OnRuntimeStop.
    ~PhysicsSystem3D();
    // Owns raw Jolt objects; a copy would release them twice.
    PhysicsSystem3D(const PhysicsSystem3D&) = delete;
    PhysicsSystem3D& operator=(const PhysicsSystem3D&) = delete;

    static void Init();
    static void Shutdown();

    void InitializeScene();
    /// Safe to call when no world exists, and more than once.
    void ShutdownScene();
    [[nodiscard]] bool IsSceneInitialized() const { return m_PhysicsSystem != nullptr; }
    void Update(Timestep ts);

    // Getters for internal Jolt classes
    JPH::PhysicsSystem* GetJoltPhysicsSystem() { return m_PhysicsSystem; }
    JPH::BodyInterface* GetBodyInterface();

    // Helper conversions
    static glm::vec3 JoltToGlm(const float* joltVec3);     // JPH::Vec3
    static glm::quat JoltToGlmQuat(const float* joltQuat); // JPH::Quat

private:
    JPH::PhysicsSystem* m_PhysicsSystem = nullptr;
    JPH::TempAllocatorImpl* m_TempAllocator = nullptr;
    JPH::JobSystemThreadPool* m_JobSystem = nullptr;

    void* m_BPLayerInterface = nullptr;
    void* m_BroadPhaseLayerFilter = nullptr;
    void* m_ObjectLayerPairFilter = nullptr;
};

} // namespace UHE::Physics
