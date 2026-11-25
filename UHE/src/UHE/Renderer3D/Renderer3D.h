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
    /**
     * @brief How models are shaded: the editor viewport's shading modes.
     *
     * Like Blender's viewport shading and Godot's view modes. Only the editor
     * changes it; a game renders Lit. The values are the shader's `viewMode`
     * (Basic3D.slang), so their order is part of that contract.
     */
    enum class ViewMode : int
    {
        Lit = 0,       ///< materials and scene lights
        Unlit = 1,     ///< base colour only
        Solid = 2,     ///< neutral clay with a studio light: shape without materials
        Normals = 3,   ///< world-space normal as colour
        Wireframe = 4, ///< triangle edges only (needs RHIDevice::SupportsWireframe)
    };

    /// @brief What the editor's ground grid shows.
    struct GridOptions
    {
        bool Axes = true;     ///< the red X and blue Z axis lines through the origin
        float Opacity = 1.0f; ///< 0..1, multiplies every line
    };

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

    /**
     * @brief Draws the ground grid (the y = 0 plane) over what is already drawn.
     *
     * Draw it after the scene: it blends on top, is hidden where geometry is
     * nearer, and writes no depth, so it never cuts into what follows.
     */
    static void DrawGrid(const GridOptions& options);
    static void DrawGrid();

    /// @brief Shading for the models submitted from now on.
    static void SetViewMode(ViewMode mode);
    [[nodiscard]] static ViewMode GetViewMode();
    /// @brief Whether ViewMode::Wireframe draws edges on this device; it draws filled otherwise.
    [[nodiscard]] static bool SupportsWireframe();

    /// Selects the pipeline set for a pass whose only attachment is the swapchain.
    ///
    /// The editor renders the scene into its own framebuffer — a colour target
    /// plus the entity-id target — while the game path renders it straight into
    /// the swapchain, which is one attachment in the swapchain's format. A
    /// pipeline has to match the pass it is bound in, so the two are separate
    /// sets rather than one.
    static void SetSwapchainTarget(bool enabled);

    /// @brief Selects the pipeline set for a pass with one colour attachment in \p format.
    ///
    /// The single-attachment set is built per format on first use: the swapchain's
    /// format for the game path, RGBA16F for a scene that renders into an HDR
    /// target before tonemapping. TextureFormat::Undefined selects the editor's
    /// two-attachment set (colour plus entity id).
    static void SetColorTarget(RHI::TextureFormat format);

    /// Rebuilds every shader module and pipeline from the library's current bytes.
    ///
    /// The shader library calls this when a source changes on disk, and the
    /// editor's Reload Shaders command calls it directly. Waits for the device to
    /// be idle first: frames already queued reference the objects being replaced.
    static void ReloadShaders();

    static bool IsLightingEnabled();
    static void SetLightingEnabled(bool enabled);

private:
    // Creates and destroys the shader modules and the pipelines built from them;
    // the two are re-created together on reload.
    static void CreateShaderResources();
    static void DestroyShaderResources();

    // Rewrites the default material (slot 0) and resets the per-frame cursor.
    // Points the light/bone/material fields at this frame-in-flight's buffers.
    static void SelectFrameBuffers();
    static void UploadLights(const std::vector<RD3d::LightData>& lights);
    static void RestartMaterialRegion();
};
} // namespace UHE
