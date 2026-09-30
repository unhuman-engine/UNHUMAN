#pragma once
#include <fastgltf/core.hpp>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include "UHE/Renderer/Texture.h"
#include "UHE/Renderer3D/Animation.h"

#include "UHE/RHI/RHITypes.h"

namespace UHE::RD3d
{

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::ivec4 jointIndices = glm::ivec4(0);
    glm::vec4 jointWeights = glm::vec4(0.0f);
};

struct Material
{
    Ref<Texture2D> AlbedoTexture = nullptr;
    Ref<Texture2D> MetallicRoughnessTexture = nullptr;
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
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

    // Generate a tangent frame for primitives carrying no TANGENT attribute.
    // Must run BEFORE vertex merging so UV-seam vertices are not merged across
    // the seam. Off by default: the vertex layout has no tangent attribute yet,
    // so the result is computed and discarded until the material work lands.
    bool generateTangents = false;
};

struct UHE_API Model
{
    Model() = default;
    ~Model();
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    bool loadModel(const std::filesystem::path& filepath, const ModelLoadOptions& options = {});
    void Destroy();

    const std::vector<Mesh>& GetMesh() const { return m_LoadedMeshes; }
    const std::vector<Material>& GetMaterials() const { return m_LoadedMaterials; }
    const std::vector<Geometry>& GetGeometry() const { return m_Geometry; }

    // True when the file declared an extension the loader does not implement, so
    // the asset renders with silently wrong shading instead of failing loudly.
    bool HasUnsupportedExtensions() const { return m_HasUnsupportedExtensions; }
    const std::vector<std::string>& GetUnsupportedExtensionNames() const { return m_UnsupportedExtensionNames; }

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
    void UploadGeometry(Geometry& geometry);
    void ReleaseGeometryBuffers(std::vector<Geometry>& geometry);

    // Implemented in LoadModelAnimation.cpp.
    void ParseSkins(const fastgltf::Asset& asset);
    void ParseAnimations(const fastgltf::Asset& asset);

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

    Skeleton m_Skeleton;
    std::vector<AnimationClip> m_Animations;

public:
    const Skeleton& GetSkeleton() const { return m_Skeleton; }
    const std::vector<AnimationClip>& GetAnimations() const { return m_Animations; }
};

} // namespace UHE::RD3d
