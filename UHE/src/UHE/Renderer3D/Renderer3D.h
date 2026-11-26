#pragma once
#include <glm/glm.hpp>
#include "UHE/Core/Core.h"
#include "UHE/RHI/RHITypes.h"
#include "UHE/Renderer/Camera.h"
#include "UHE/Renderer/EditorCamera.h"
#include "UHE/Renderer3D/Animator.h"
#include "UHE/Renderer3D/LightSystem.h"
#include "UHE/Renderer3D/LoadModel.h"

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
        Lit = 0,             ///< materials and scene lights
        Unlit = 1,           ///< base colour only
        Solid = 2,           ///< neutral clay with a studio light: shape without materials
        Normals = 3,         ///< world-space normal as colour
        Wireframe = 4,       ///< triangle edges only (needs RHIDevice::SupportsWireframe)
        Overdraw = 5,        ///< how often each pixel is drawn: every fragment adds, nothing hides anything
        LightComplexity = 6, ///< how many local lights reach each pixel, as a heatmap
    };

    /// @brief What the editor's ground grid shows.
    struct GridOptions
    {
        bool Axes = true;     ///< the red X and blue Z axis lines through the origin
        float Opacity = 1.0f; ///< 0..1, multiplies every line
    };

    /**
     * @brief Constants shared by every model draw of a frame: Basic3D.slang's FrameData.
     *
     * Shadows, ambient light, occlusion and wind live here rather than in push
     * constants, which are close to the device's 256-byte limit. A draw without
     * frame data (ClearFrameData) shades as before: flat ambient, no shadows,
     * and the shader's own tonemap for a target that is not HDR.
     *
     * MUST match FrameData in Basic3D.slang: vec4-aligned, std430.
     */
    struct SceneFrameData
    {
        glm::mat4 CascadeViewProj[4]{}; ///< world to shadow clip space, per cascade
        glm::vec4 CascadeSplits{0.0f};  ///< view distance where each cascade ends
        glm::vec4 CascadeRects[4]{};    ///< where each cascade sits in the atlas: xy offset, zw scale (uv)
        glm::vec4 CascadeTexel{0.0f};   ///< world size of one shadow texel, per cascade
        glm::vec4 Shadow{-1.0f, -1.0f, 0.0f,
                         0.0f};     ///< x: atlas texture (-1 none), y: light index, z: texel size (uv), w: cascades
        glm::vec4 ShadowBias{0.0f}; ///< x: bias toward the light, y: along the normal (texels), z: filter radius
                                    ///< (texels), w: show cascades
        glm::vec4 CameraForward{0.0f, 0.0f, -1.0f, 0.0f}; ///< xyz: view direction, for the cascade a pixel falls in
        glm::vec4 AmbientSky{0.08f};                      ///< linear, already scaled by the intensity
        glm::vec4 AmbientGround{0.08f};
        glm::vec4 Screen{0.0f, 0.0f, -1.0f,
                         1.0f}; ///< xy: 1 / target size, z: occlusion texture (-1 none), w: its strength
        glm::vec4 Wind{0.0f};   ///< xy: steady wind on x/z, z: gust strength, w: time in seconds
        glm::vec4 Flags{0.0f};  ///< x: 1 when the target is HDR (no tonemap in the shader), y: gust frequency
        /// rgb: sky colour (linear, unscaled), w: first directional light (-1 none)
        glm::vec4 Sky{0.0f, 0.0f, 0.0f, -1.0f};
        /// x: local shadow atlas texture (-1 none), y: tile count, z: tile buffer (bindless)
        glm::vec4 LocalShadow{-1.0f, 0.0f, -1.0f, 0.0f};
        glm::vec4 LocalShadowBias{0.0f}; ///< x: toward the light, y: along the normal (texels)
    };

    /**
     * @brief One tile of the local shadow atlas: a spot or area light, or one cube face of a point light.
     *
     * MUST match ShadowTile in Basic3D.slang.
     */
    struct ShadowTile
    {
        glm::mat4 ViewProj{1.0f};
        glm::vec4 Rect{0.0f};   ///< xy: offset, zw: scale, in atlas uv
        glm::vec4 Params{0.0f}; ///< x: light index, y: cube face (-1 spot/area), z: texel size per metre of distance
    };
    /// @brief Uploads this frame's local shadow tiles. @return the buffer's bindless index, for SceneFrameData.
    static int SetShadowTiles(const std::vector<ShadowTile>& tiles);

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

    /**
     * @brief The bone matrices for \p model: the animator's while it plays a
     *        clip, the skin's rest pose otherwise (RD3d::Skin::RestPalette).
     *
     * A skinned model without an animation still needs its skeleton: glTF
     * places a skinned mesh by its joints and ignores the mesh node's own
     * transform, which in many exports turns the model on its side.
     */
    static BoneBinding PrepareBoneBinding(const RD3d::Model& model, const RD3d::Animator* animator);

    /**
     * @brief The bone matrices for the meshes of \p model that use skin \p skinIndex.
     *
     * The animator's matrices while it plays a clip on that skin, the skin's
     * rest pose otherwise; nothing for a rigid mesh (\p skinIndex < 0), which
     * the shader would otherwise skin with zero weights and collapse to the
     * origin. A model with several skins needs one binding per skin.
     */
    static BoneBinding PrepareSkinBinding(const RD3d::Model& model, const RD3d::Animator* animator, int skinIndex);

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

    /**
     * @brief Colour format of the two-attachment target (colour plus entity IDs).
     *
     * RGBA8_SRGB by default (what Undefined selects); the editor renders its
     * scene into RGBA16F and tonemaps it afterwards. Pipelines for a new
     * format are built on first use.
     */
    static void SetEntityTargetFormat(RHI::TextureFormat format);

    /// Rebuilds every shader module and pipeline from the library's current bytes.
    ///
    /// The shader library calls this when a source changes on disk, and the
    /// editor's Reload Shaders command calls it directly. Waits for the device to
    /// be idle first: frames already queued reference the objects being replaced.
    static void ReloadShaders();

    /**
     * @brief Uploads \p data for this frame; every model drawn from now on reads it.
     *
     * Can be called again within the frame (each pass that knows more fills in
     * more): the buffer is host-visible and read when the frame executes, so
     * every draw of the frame sees the last values written.
     */
    static void SetFrameData(const SceneFrameData& data);
    /// @brief Draws from now on have no frame data (the game path, the sandbox).
    static void ClearFrameData();
    /// @brief Bindless index of this frame's SceneFrameData buffer, for passes with their own shaders (water).
    [[nodiscard]] static int GetFrameDataBufferIndex();
    /// @brief Bindless index of this frame's light buffer (the lights of the last BeginScene).
    [[nodiscard]] static int GetLightBufferIndex();

    /**
     * @brief Submits from now on write depth only, for a shadow map or a depth prepass.
     *
     * Blended and transmissive materials are skipped (they write no depth in
     * the colour pass either), and alpha-masked ones keep their cutout. The
     * vertex stage is the colour pass's own, so a prepass and the colour pass
     * that follows it compute the same depth.
     * @param depthFormat D32_FLOAT for a shadow map (no culling), D24_UNORM_S8 for the scene prepass
     */
    static void BeginDepthPass(const glm::mat4& viewProjection, RHI::TextureFormat depthFormat);
    static void EndDepthPass();

    /**
     * @brief Wind bending for the models submitted from now on (vegetation).
     * @param weight 0 keeps the model rigid; 1 bends it fully with the scene's wind
     * @param height model-space height at which the bend reaches full strength
     */
    static void SetFoliage(float weight, float height);

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
