#pragma once
#include <fastgltf/core.hpp>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include "UHE/Renderer/Texture.h"
#include "UHE/Renderer3D/Animation.h"
#include "UHE/Renderer3D/MaterialExtensions.h"

#include "UHE/RHI/RHITypes.h"

namespace UHE::RD3d
{

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    // xyz = tangent, w = handedness (+1/-1). Required for normal mapping.
    glm::vec4 tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    glm::ivec4 jointIndices = glm::ivec4(0);
    glm::vec4 jointWeights = glm::vec4(0.0f);

    // COLOR_0, as vec4. Defaults to WHITE with alpha 1, which is the glTF
    // semantic for "no COLOR_0": the shader multiplies this into base colour, so
    // a black or zero default would tint every mesh in the engine black.
    // glTF permits vec3 or vec4 here; vec4 covers both, since a vec3 accessor
    // reads alpha as 1 - one type on the GPU side rather than two.
    glm::vec4 color = glm::vec4(1.0f);
};

// How a material's alpha is interpreted. glTF alphaMode.
enum class AlphaMode : u32
{
    Opaque = 0,  // alpha ignored
    Mask = 1,    // fragment discarded below alphaCutoff
    Blend = 2,   // alpha blended (needs sorted transparent pass - not implemented)
};

// KHR_texture_transform, already converted to the engine's UV convention.
//
// The loader flips V when importing TEXCOORD_0 (see LoadModelGeometry.cpp), so
// the raw glTF offset/rotation cannot be applied in the shader on top of
// flipped coordinates. LoadMaterials stores the ENGINE-SPACE parameters: the
// shader computes  uv' = R(rot) * (uv * scale) + offset  in that same space.
// rot carries cos/sin of the adjusted angle; identity is (1, 0, 1, 1, 0, 0).
struct UVTransform
{
    float cosRotation = 1.0f;
    float sinRotation = 0.0f;
    glm::vec2 scale = glm::vec2(1.0f);
    glm::vec2 offset = glm::vec2(0.0f);
    // glTF texCoordIndex the map applies to. Only TEXCOORD_0 is extracted, so
    // any other value cannot be honoured; the loader warns and stays on UV0.
    u32 texCoordSet = 0;
    bool HasTransform = false;
};

// The per-material texture slots a transform can ride on, in the order the
// renderer's per-material GPU buffer stores them (MaterialGPU.h mirrors this).
enum class MaterialTextureSlot : int
{
    Albedo = 0,
    MetallicRoughness,
    Normal,
    Occlusion,
    Emissive,
    Clearcoat,
    ClearcoatRoughness,
    ClearcoatNormal,
    Specular,
    SpecularColor,
    SheenColor,
    SheenRoughness,
    Transmission,
    Thickness,
    Iridescence,
    IridescenceThickness,
    Anisotropy,
    DiffuseTransmission,
    DiffuseTransmissionColor,
    Count
};

struct Material
{
    Ref<Texture2D> AlbedoTexture = nullptr;
    Ref<Texture2D> MetallicRoughnessTexture = nullptr;
    Ref<Texture2D> NormalTexture = nullptr;
    Ref<Texture2D> OcclusionTexture = nullptr;
    Ref<Texture2D> EmissiveTexture = nullptr;

    // baseColorFactor. glTF defaults to opaque white (1,1,1,1).
    glm::vec4 BaseColorFactor = glm::vec4(1.0f);

    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;

    // normalTexture.scale defaults to 1.0, NOT 0.0 - a zero default silently
    // disables every normal map in the scene.
    float NormalScale = 1.0f;

    // occlusionTexture.strength defaults to 1.0.
    float OcclusionStrength = 1.0f;

    // emissiveFactor defaults to BLACK (0,0,0), not white. Defaulting to 1.0
    // here makes every unlit emissive surface glow.
    glm::vec3 EmissiveFactor = glm::vec3(0.0f);

    AlphaMode Alpha = AlphaMode::Opaque;
    // alphaCutoff defaults to 0.5 per spec.
    float AlphaCutoff = 0.5f;

    // Controls pipeline cull mode. Cull mode is baked into the pipeline, so this
    // selects between two pre-built pipeline variants rather than a dynamic state.
    bool DoubleSided = false;

    // KHR_texture_transform, per texture slot. Most files carry at most one
    // transform (usually on baseColor), so the array costs little and keeps the
    // shader's slot addressing uniform: every slot is sampled through its own
    // transform, and a slot without one reads identity.
    UVTransform UVTransforms[static_cast<size_t>(MaterialTextureSlot::Count)] = {};

    // Optional PBR extensions (issue #29 Tier 2 and Tier 3). Kept in one aggregate
    // rather than spread across Material so "which extensions did this material
    // actually use" is one question, and so a material declaring none of them
    // renders byte-identically to one from before this struct existed.
    MaterialExtensions Extensions;
};

// One drawable range over a geometry buffer.
//
// Primitives no longer own their GPU buffers outright. Several nodes can
// reference the same glTF mesh (foliage, repeated props); the loader extracts
// such a mesh once into Geometry, and each node gets a Primitive holding a
// range into that shared Geometry. This is what makes per-node LocalTransform
// viable - duplicating the vertices per node would defeat the point.
struct Primitive
{
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    size_t materialIndex = 0;

    // True when this primitive carried a COLOR_0 attribute. Carried here, on the
    // Primitive, rather than as a field on Vertex: it is one fact about the whole
    // primitive, and a per-vertex flag would grow the stride that the vertex-merge
    // pass hashes over. The shader multiplies vertex colour only when this is set,
    // which matters for glTF's own rule that COLOR_0 is linear data and so must
    // NOT be gamma-decoded - the flag is what keeps that decision on the CPU side.
    bool hasVertexColor = false;

    // Range into Model::GetGeometry(). Only meaningful on the owning Primitive.
    size_t geometryIndex = 0;
    // True on every Primitive that borrows geometry owned by another Primitive,
    // i.e. every node after the first that references a given glTF mesh.
    bool usesSharedGeometry = false;

    // RHI resources. Only the owning Primitive sets these.
    RHI::BufferHandle VertexBuffer = nullptr;
    RHI::BufferHandle IndexBuffer = nullptr;
    u32 IndexCount = 0;
};

// CPU-side geometry for one glTF mesh: extracted once, uploaded once.
struct Geometry
{
    std::string name;
    std::vector<Primitive> primitive;

    // Axis-aligned bounds in the mesh's OWN space (node transforms NOT applied).
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
    bool hasBounds = false;
};

// One node of the glTF scene graph: a drawable instance of a Geometry, placed by
// its accumulated world transform.
//
// The type keeps its historical name and keeps `primitive` as a member, so
// existing render and editor code compiles unchanged.
struct Mesh
{
    // Node name when the file provided one, else the mesh name, else a fallback.
    std::string name;
    // Node index in the source asset; SIZE_MAX for the owning entry.
    size_t nodeIndex = SIZE_MAX;
    size_t geometryIndex = 0;

    // Accumulated transform of every node from the scene root down to and
    // including this one. Deliberately NOT baked into vertices:
    // Renderer3D::SubmitModel multiplies the entity transform by this at submit
    // time, which keeps skinning correct (bone matrices still apply on top) and
    // needs no vertex rewriting.
    glm::mat4 LocalTransform{1.0f};

    std::vector<Primitive> primitive;

    // World-space bounds: geometry bounds with LocalTransform applied.
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);
    bool hasBounds = false;

    // True on the first node to reference this geometry; that entry owns the
    // GPU buffers and Destroy() frees them exactly once.
    bool ownsGeometryBuffers = false;

    bool IsDrawable() const { return hasBounds && !primitive.empty(); }
};

// Which loader passes to run.
struct ModelLoadOptions
{
    // Reorder indices for vertex-cache and vertex-fetch efficiency. Lossless -
    // only permutation - and it matters on the GFX9 vertex-fetch floor.
    bool optimizeMesh = true;

    // Merge byte-identical vertices. Kept separate from optimizeMesh because it
    // CANNOT be applied to skinned primitives: merging vertices that belong to
    // different joints destroys the skinning.
    bool mergeVertices = true;

    // Generate a tangent frame for primitives that carry no TANGENT attribute.
    //
    // Must run BEFORE vertex merging: merging hashes every byte of a vertex, so
    // seam vertices that differ only in UV would collapse into one, and the
    // tangent frame at that vertex would then be meaningless. The loader
    // enforces this ordering internally regardless of the values here.
    bool generateTangents = true;
};

// Issue #17: flat representation of the glTF node tree so the editor can turn
// each node into a controllable child entity instead of one opaque model.
struct ModelNode
{
    std::string Name;
    int Parent = -1;          // index into Model::GetNodes(), -1 for roots
    std::vector<int> Children;
    int MeshIndex = -1;       // index into Model::GetMesh(), -1 if empty node

    glm::vec3 Translation{0.0f};
    glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 Scale{1.0f};

    // KHR_lights_punctual: index into Model::GetPunctualLights(), -1 when the
    // node carries no light.
    int LightIndex = -1;

    // The loader's accumulated model-space transform (every ancestor composed).
    // Used to place node-carried lights without re-walking the tree; the editor
    // hierarchy derives transforms from the TRS fields above instead.
    glm::mat4 WorldTransform{1.0f};
};

// One KHR_lights_punctual light, as declared in asset.extensions - the scene
// placement lives on the nodes, this is only the light's own definition.
struct PunctualLight
{
    enum class Type : u8
    {
        Directional = 0,
        Spot = 1,
        Point = 2,
    };

    Type type = Type::Point;
    // Linear-space colour and the unit the spec assigns per type: candela for
    // point/spot, lux for directional.
    glm::vec3 Color = glm::vec3(1.0f);
    float Intensity = 1.0f;
    // Point/spot only. INFINITY per spec when the file omitted it - the loader
    // resolves that to the engine's "no range limit" rather than storing a
    // value that would clamp every light to zero reach.
    float Range = std::numeric_limits<float>::infinity();
    // Spot only, radians. innerConeAngle defaults to 0, outerConeAngle to
    // PI/4 per spec.
    float InnerConeAngle = 0.0f;
    float OuterConeAngle = glm::pi<float>() / 4.0f;

    std::string Name;
};

class UHE_API Model
{
public:
    Model() = default;
    ~Model();
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    bool loadModel(const std::filesystem::path& filepath, const ModelLoadOptions& options = {});
    void Destroy();

    const std::vector<Mesh>& GetMesh() const { return m_LoadedMeshes; }
    const std::vector<Material>& GetMaterials() const { return m_LoadedMaterials; }
    const std::vector<Geometry>& GetGeometry() const { return m_Geometry; }

    // True when any material in the file uses alphaMode BLEND. Transparent
    // materials need a depth-sorted pass, which does not exist yet (roadmap M3
    // step 4), so the renderer can warn instead of silently drawing them in
    // submission order.
    bool HasTransparentMaterials() const { return m_HasTransparentMaterials; }

    // True when the file declared an extension the loader does not implement, so
    // the asset renders with subtly wrong shading instead of failing loudly.
    bool HasUnsupportedExtensions() const { return m_HasUnsupportedExtensions; }
    const std::vector<std::string>& GetUnsupportedExtensionNames() const { return m_UnsupportedExtensionNames; }
    // Flat glTF node list; roots are the entries with Parent == -1.
    const std::vector<ModelNode>& GetNodes() const { return m_Nodes; }
    const std::vector<int>& GetRootNodes() const { return m_RootNodes; }

    // KHR_lights_punctual (issue #29 Tier 3). Empty for files that declare no
    // lights; nodes reference entries by ModelNode::LightIndex.
    const std::vector<PunctualLight>& GetPunctualLights() const { return m_PunctualLights; }


private:
    void LoadMaterials(const fastgltf::Asset& asset, const std::filesystem::path& filepath);
    void ProcessNode(const fastgltf::Asset& asset, size_t nodeIndex, const glm::mat4& parentTransform);
    void ComputeBounds(Geometry& geometry);

    // Implemented in LoadModelGeometry.cpp - CPU-side only, no RHI types touched
    // beyond the buffer handles it leaves at zero. Kept in its own translation
    // unit because it pulls in meshoptimizer and fastgltf accessor iteration,
    // which is the bulk of the loader's compile time.
    void ExtractGeometry(const fastgltf::Asset& asset, size_t meshIndex, const ModelLoadOptions& options);

    // Implemented in LoadModelUpload.cpp - the only place that creates GPU buffers.
    void UploadGeometry(Geometry& geometry, size_t geometryIndex);
    void ReleaseGeometryBuffers(std::vector<Geometry>& geometry);

    // Implemented in LoadModelAnimation.cpp.
    void ParseSkins(const fastgltf::Asset& asset);
    void ParseAnimations(const fastgltf::Asset& asset);

    // KHR_lights_punctual: the light definitions of asset.extensions. Node
    // placement (node.lightIndex) is resolved later, in ProcessNode.
    void ParsePunctualLights(const fastgltf::Asset& asset);

    static glm::mat4 NodeLocalTransform(const fastgltf::Node& node);

private:
    std::vector<Mesh> m_LoadedMeshes;
    std::vector<Material> m_LoadedMaterials;
    std::vector<Geometry> m_Geometry;

    // glTF meshIndex -> index into m_Geometry. Nodes sharing a mesh share
    // geometry, so the mesh is extracted and uploaded only once.
    std::unordered_map<size_t, size_t> m_GeometryCache;

    // Options for the load in progress; ProcessNode needs mergeVertices to decide
    // whether a geometry is safe to merge, so it cannot be a pure parameter.
    ModelLoadOptions m_Options;

    std::vector<std::string> m_UnsupportedExtensionNames;
    bool m_HasUnsupportedExtensions = false;
    bool m_HasTransparentMaterials = false;
    std::vector<ModelNode> m_Nodes;
    std::vector<int> m_RootNodes;
    std::unordered_map<int, int> m_NodeToMesh; // glTF node -> m_LoadedMeshes index
    std::vector<PunctualLight> m_PunctualLights;


    Skeleton m_Skeleton;
    std::vector<AnimationClip> m_Animations;

public:
    const Skeleton& GetSkeleton() const { return m_Skeleton; }
    const std::vector<AnimationClip>& GetAnimations() const { return m_Animations; }
};

} // namespace UHE::RD3d
