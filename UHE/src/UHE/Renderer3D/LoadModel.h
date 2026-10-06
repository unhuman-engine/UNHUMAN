#pragma once
#include <fastgltf/core.hpp>
#include <filesystem>
#include "fastgltf/types.hpp"
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

struct Primitive
{
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    size_t materialIndex = 0;

    // RHI resources
    RHI::BufferHandle VertexBuffer = nullptr;
    RHI::BufferHandle IndexBuffer = nullptr;
    uint32_t IndexCount = 0;
};

struct Mesh
{
    std::string name;
    std::vector<Primitive> primitive;
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
};

class UHE_API Model
{
public:
    Model() = default;
    ~Model();
    bool loadModel(const std::filesystem::path& filepath);
    void Destroy();

    const std::vector<Mesh>& GetMesh() const { return m_LoadedMeshes; }
    const std::vector<Material>& GetMaterials() const { return m_LoadedMaterials; }
    // Flat glTF node list; roots are the entries with Parent == -1.
    const std::vector<ModelNode>& GetNodes() const { return m_Nodes; }
    const std::vector<int>& GetRootNodes() const { return m_RootNodes; }

private:
    void ProcessNode(const fastgltf::Asset& asset, size_t nodeIndex);
    void ExtractMesh(const fastgltf::Asset& asset, const fastgltf::Mesh& gltfMesh);
    void ParseSkins(const fastgltf::Asset& asset);
    void ParseAnimations(const fastgltf::Asset& asset);

private:
    std::vector<Mesh> m_LoadedMeshes;
    std::vector<Material> m_LoadedMaterials;
    std::vector<ModelNode> m_Nodes;
    std::vector<int> m_RootNodes;
    std::unordered_map<int, int> m_NodeToMesh; // glTF node -> m_LoadedMeshes index

    Skeleton m_Skeleton;
    std::vector<AnimationClip> m_Animations;
    std::vector<Skin> m_Skins; // Issue #41: all skins, not just the first

public:
    const Skeleton& GetSkeleton() const { return m_Skeleton; }
    const std::vector<AnimationClip>& GetAnimations() const { return m_Animations; }
    // Issue #41: multi-skin access. GetSkin returns nullptr when the model
    // has no skins or the index is out of range.
    const std::vector<Skin>& GetSkins() const { return m_Skins; }
    size_t GetSkinCount() const { return m_Skins.size(); }
    const Skin* GetSkin(int index) const
    {
        if (index < 0 || index >= static_cast<int>(m_Skins.size()))
            return nullptr;
        return &m_Skins[index];
    }

    // Issue #41: the animation runtime must not depend on glTF. These let a
    // cooked/imported model (or a test) populate the runtime data directly.
    void SetSkeleton(const Skeleton& skeleton) { m_Skeleton = skeleton; }
    void AddAnimation(const AnimationClip& clip) { m_Animations.push_back(clip); }
    void AddSkin(const Skin& skin) { m_Skins.push_back(skin); }
};

} // namespace UHE::RD3d
