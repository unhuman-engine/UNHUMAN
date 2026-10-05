#pragma once
#include "UHE/Core/Core.h"
#include "UHE/Renderer3D/LoadModel.h"
#include "UHE/Renderer3D/Animator.h"
#include "UHE/Renderer3D/LightSystem.h"
#include "UHE/Renderer/EditorCamera.h"
#include "UHE/Renderer/Camera.h"
#include <glm/glm.hpp>

namespace UHE
{
class UHE_API Renderer3D
{
public:
    // Bone buffer location for one model's animation upload this frame.
    struct BoneBinding
    {
        int BufferIndex = -1;
        int Offset = -1;
    };

    // Uploads the animator's bone matrices once and returns where they live;
    // pass the result to every SubmitMesh of that model so expanded glTF node
    // entities share one binding instead of losing skinning.
    static BoneBinding PrepareBoneBinding(const RD3d::Animator* animator);
    static void Init();
    static void Shutdown();

    static void BeginScene(const EditorCamera& camera, const std::vector<RD3d::LightData>& lights);
    static void BeginScene(const Camera& camera, const glm::mat4& transform, const std::vector<RD3d::LightData>& lights);
    static void EndScene();

    static void SubmitModel(const RD3d::Model& model, const glm::mat4& transform = glm::mat4(1.0f), int entityID = -1, const RD3d::Animator* animator = nullptr);
    // Issue #17: draw a single mesh (one glTF node's sub-meshes) with the
    // materials of its parent model. Bone constants are prepared by SubmitModel
    // (animation upload happens once per model) and forwarded here.
    static void SubmitMesh(const RD3d::Mesh& mesh, const glm::mat4& transform, int entityID = -1,
                           const std::vector<RD3d::Material>& materials = {}, int boneBufferIndex = -1,
                           int boneOffset = -1);
    
    static void DrawGrid();
    
    static bool IsLightingEnabled();
    static void SetLightingEnabled(bool enabled);
};
} // namespace UHE
