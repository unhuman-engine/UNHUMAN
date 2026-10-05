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

    // Where one model's materials live in the renderer's per-frame material
    // buffer (see MaterialGPU.h). BaseIndex -1 means the model has no
    // materials; Count caps what actually uploaded when the per-frame region
    // ran out, so trailing primitives can fall back to the default material.
    struct MaterialBinding
    {
        int BufferIndex = -1;
        int BaseIndex = -1;
        int Count = 0;
    };

    // Uploads the animator's bone matrices once and returns where they live;
    // pass the result to every SubmitMesh of that model so expanded glTF node
    // entities share one binding instead of losing skinning.
    static BoneBinding PrepareBoneBinding(const RD3d::Animator* animator);

    // Uploads the model's material parameters once per frame and returns the
    // buffer location; pass the result to every SubmitMesh of that model.
    static MaterialBinding PrepareMaterialBinding(const RD3d::Model& model);

    static void Init();
    static void Shutdown();

    static void BeginScene(const EditorCamera& camera, const std::vector<RD3d::LightData>& lights);
    static void BeginScene(const Camera& camera, const glm::mat4& transform, const std::vector<RD3d::LightData>& lights);
    static void EndScene();

    static void SubmitModel(const RD3d::Model& model, const glm::mat4& transform = glm::mat4(1.0f), int entityID = -1, const RD3d::Animator* animator = nullptr);
    // Issue #17: draw a single mesh (one glTF node's sub-meshes). Bone and
    // material bindings are prepared once per model (PrepareBoneBinding /
    // PrepareMaterialBinding) and forwarded here, so expanded node entities
    // share one upload instead of re-uploading per node.
    static void SubmitMesh(const RD3d::Mesh& mesh, const glm::mat4& transform, int entityID,
                           const RD3d::Model& model, const MaterialBinding& materialBinding,
                           int boneBufferIndex = -1, int boneOffset = -1);

    static void DrawGrid();

    static bool IsLightingEnabled();
    static void SetLightingEnabled(bool enabled);

private:
    // Rewrites the default material (slot 0) and resets the per-frame cursor.
    static void RestartMaterialRegion();
};
} // namespace UHE
