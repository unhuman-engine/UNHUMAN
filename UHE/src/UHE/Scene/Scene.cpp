#include "uhepch.h"
#include "Scene.h"
#include <box2d/box2d.h>
#include <glm/glm.hpp>
#include "Components.h"
#include "Entity.h"
#include "ScriptableEntity.h"
#include "UHE/Core/UIID.h"
#include "UHE/Renderer/Renderer2D.h"
#include "UHE/Renderer3D/Renderer3D.h"
#include "UHE/Renderer2D/SubTexture2D.h"
#include "UHE/Renderer3D/LightSystem.h"
#include "UHE/AssestsManager/VfsSystem.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

// Jolt Physics
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>

namespace UHE
{

Scene::Scene()
{
    // Initialize all component pools to prevent EnTT out-of-bounds asserts across DLL boundaries
    m_registry.storage<IDComponent>();
    m_registry.storage<TagComponent>();
    m_registry.storage<TransformComponent>();
    m_registry.storage<CameraComponent>();
    m_registry.storage<SpriteRendererComponent>();
    m_registry.storage<TextComponent>();
    m_registry.storage<SpriteAnimationComponent>();
    m_registry.storage<NativeScriptComponent>();
    m_registry.storage<RigidBody2DComponent>();
    m_registry.storage<BoxColliderComponent>();
    m_registry.storage<Model3DComponent>();
    m_registry.storage<RigidBody3DComponent>();
    m_registry.storage<BoxCollider3DComponent>();
    m_registry.storage<SphereCollider3DComponent>();
    m_registry.storage<CapsuleCollider3DComponent>();
    m_registry.storage<ModelNodeComponent>();
    m_registry.storage<RelationshipComponent>();
}

Scene::~Scene()
{
    // Issue #17 lifetime fix: release script instances before the registry
    // dies so OnDestroy() can still access components and handles safely.
    DestroyScriptInstances();
}

void Scene::DestroyScriptInstances()
{
    // ScriptableEntity grants Scene friendship, so calling OnDestroy() here
    // is allowed even though it is protected.
    m_registry.view<NativeScriptComponent>().each(
        [this](auto entity, auto& nsc)
        {
            if (nsc.Instance)
            {
                nsc.Instance->OnDestroy();
                if (nsc.DestroyScript)
                    nsc.DestroyScript(&nsc);
                else
                    delete nsc.Instance; // bound scripts always set DestroyScript
                nsc.Instance = nullptr;
            }
        });
}

Ref<Scene> Scene::Copy(Ref<Scene> other)
{
    Ref<Scene> newScene = CreateRef<Scene>();
    newScene->m_ViewportWidth = other->m_ViewportWidth;
    newScene->m_ViewportHeight = other->m_ViewportHeight;

    auto& srcRegistry = other->m_registry;
    auto& dstRegistry = newScene->m_registry;

    auto view = srcRegistry.view<TagComponent>();
    for (auto srcEntity : view)
    {
        const auto& tag = srcRegistry.get<TagComponent>(srcEntity).Tag;
        Entity newEntity = newScene->CreateEntity(tag);

        // Issue #17: keep the same UUID so parent/child references survive the
        // copy (play mode snapshot).
        if (srcRegistry.all_of<IDComponent>(srcEntity))
        {
            u64 generatedID = newEntity.GetUUID();
            newEntity.GetComponent<IDComponent>().ID = srcRegistry.get<IDComponent>(srcEntity).ID;
            newScene->UnindexEntity(generatedID); // drop the auto-generated key
            newScene->IndexEntity(newEntity, newEntity.GetUUID());
        }
        if (srcRegistry.all_of<RelationshipComponent>(srcEntity))
            newEntity.GetComponent<RelationshipComponent>() = srcRegistry.get<RelationshipComponent>(srcEntity);

        // Copy TransformComponent
        if (srcRegistry.all_of<TransformComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<TransformComponent>(srcEntity);
            auto& dst = newEntity.GetComponent<TransformComponent>();
            dst.Translation = src.Translation;
            dst.Rotation = src.Rotation;
            dst.Scale = src.Scale;
        }

        // Copy CameraComponent
        if (srcRegistry.all_of<CameraComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<CameraComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<CameraComponent>();
            dst.Camera = src.Camera;
            dst.Primary = src.Primary;
            dst.FixedAspectRatio = src.FixedAspectRatio;
        }

        // Copy SpriteRendererComponent
        if (srcRegistry.all_of<SpriteRendererComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<SpriteRendererComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<SpriteRendererComponent>();
            dst.Color = src.Color;
            dst.TexturePath = src.TexturePath;
            dst.Texture = src.Texture;
            dst.TilingFactor = src.TilingFactor;
            dst.UseSubTexture = src.UseSubTexture;
            dst.SubTextureCoords = src.SubTextureCoords;
            dst.SubTextureCellSize = src.SubTextureCellSize;
            dst.SubTextureSpriteSize = src.SubTextureSpriteSize;
        }

        // Copy TextComponent
        if (srcRegistry.all_of<TextComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<TextComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<TextComponent>();
            dst.TextString = src.TextString;
            dst.FontAsset = src.FontAsset;
            dst.Color = src.Color;
            dst.Kerning = src.Kerning;
            dst.LineSpacing = src.LineSpacing;
        }


        // Copy SpriteAnimationComponent
        if (srcRegistry.all_of<SpriteAnimationComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<SpriteAnimationComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<SpriteAnimationComponent>();
            dst.SpriteSheetPath = src.SpriteSheetPath;
            dst.Animation = src.Animation;
            dst.Color = src.Color;
        }

        // Copy NativeScriptComponent
        if (srcRegistry.all_of<NativeScriptComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<NativeScriptComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<NativeScriptComponent>();
            dst.InstantiateScript = src.InstantiateScript;
            dst.DestroyScript = src.DestroyScript;
        }

        // Copy RigidBody2DComponent
        if (srcRegistry.all_of<RigidBody2DComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<RigidBody2DComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<RigidBody2DComponent>();
            dst.Type = src.Type;
            dst.FixedRotation = src.FixedRotation;
        }

        // Copy BoxColliderComponent
        if (srcRegistry.all_of<BoxColliderComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<BoxColliderComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<BoxColliderComponent>();
            dst.Offset = src.Offset;
            dst.Size = src.Size;
            dst.Density = src.Density;
            dst.Friction = src.Friction;
            dst.Restitution = src.Restitution;
            dst.RestitutionThreshold = src.RestitutionThreshold;
        }
        // Copy Model3DComponent
        if (srcRegistry.all_of<Model3DComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<Model3DComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<Model3DComponent>();
            dst.ModelPath = src.ModelPath;
            dst.IsLoaded = src.IsLoaded;
            dst.ModelData = src.ModelData;
        }

        // Issue #17: Copy ModelNodeComponent
        if (srcRegistry.all_of<ModelNodeComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<ModelNodeComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<ModelNodeComponent>();
            dst.ModelEntity = src.ModelEntity;
            dst.NodeIndex = src.NodeIndex;
            dst.NodeName = src.NodeName;
            dst.HasChildrenNodes = src.HasChildrenNodes;
        }

        // Copy AnimatorComponent
        if (srcRegistry.all_of<AnimatorComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<AnimatorComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<AnimatorComponent>();
            dst.CurrentAnimationName = src.CurrentAnimationName;
            dst.PlaybackSpeed = src.PlaybackSpeed;
            dst.IsPlaying = src.IsPlaying;
            // Issue #41 playback/blending controls.
            dst.LoopMode = src.LoopMode;
            dst.Reverse = src.Reverse;
            dst.RootMotion = src.RootMotion;
            dst.CrossFadeDuration = src.CrossFadeDuration;
            dst.SkinIndex = src.SkinIndex;
            // Note: Animator itself needs to be recreated since it depends on the ModelData instance,
            // but we can just share it or let it re-initialize in OnUpdate. We'll copy the ref.
            dst.Animator = src.Animator;
        }

        // Copy DirectionalLightComponent
        if (srcRegistry.all_of<DirectionalLightComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<DirectionalLightComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<DirectionalLightComponent>();
            dst.Color = src.Color;
            dst.Intensity = src.Intensity;
        }

        // Copy PointLightComponent
        if (srcRegistry.all_of<PointLightComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<PointLightComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<PointLightComponent>();
            dst.Color = src.Color;
            dst.Intensity = src.Intensity;
            dst.Radius = src.Radius;
        }

        // Copy RigidBody3DComponent
        if (srcRegistry.all_of<RigidBody3DComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<RigidBody3DComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<RigidBody3DComponent>();
            dst.Type = src.Type;
            dst.Mass = src.Mass;
            dst.LinearDamping = src.LinearDamping;
            dst.AngularDamping = src.AngularDamping;
            for (int i = 0; i < 6; i++) dst.AllowedDOFs[i] = src.AllowedDOFs[i];
            dst.IsSensor = src.IsSensor;
        }

        // Copy BoxCollider3DComponent
        if (srcRegistry.all_of<BoxCollider3DComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<BoxCollider3DComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<BoxCollider3DComponent>();
            dst.Offset = src.Offset;
            dst.HalfExtent = src.HalfExtent;
            dst.Friction = src.Friction;
            dst.Restitution = src.Restitution;
        }

        // Copy SphereCollider3DComponent
        if (srcRegistry.all_of<SphereCollider3DComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<SphereCollider3DComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<SphereCollider3DComponent>();
            dst.Offset = src.Offset;
            dst.Radius = src.Radius;
            dst.Friction = src.Friction;
            dst.Restitution = src.Restitution;
        }

        // Copy CapsuleCollider3DComponent
        if (srcRegistry.all_of<CapsuleCollider3DComponent>(srcEntity))
        {
            auto& src = srcRegistry.get<CapsuleCollider3DComponent>(srcEntity);
            auto& dst = newEntity.AddComponent<CapsuleCollider3DComponent>();
            dst.Offset = src.Offset;
            dst.Radius = src.Radius;
            dst.HalfHeight = src.HalfHeight;
            dst.Friction = src.Friction;
            dst.Restitution = src.Restitution;
        }
    }

    return newScene;
}

UHE::Entity Scene::CreateEntity(const std::string& name /*= std::string()*/)
{
    Entity entity = {m_registry.create(), this};
    entity.AddComponent<IDComponent>();
    entity.AddComponent<TransformComponent>();
    entity.AddComponent<RelationshipComponent>(); // Issue #17
    auto& tag = entity.AddComponent<TagComponent>();
    tag.Tag = name.empty() ? "Entity" : name;

    IndexEntity(entity, entity.GetUUID());

    return entity;
}

UHE::Entity Scene::CreateChildEntity(Entity parent, const std::string& name /*= std::string()*/)
{
    // Defer the actual attach: the editor calls this while iterating the
    // hierarchy UI, and emplacing into a component pool would invalidate
    // those iterators (entt swaps storage on emplace).
    if (!parent)
        return CreateEntity(name);

    Entity entity = CreateEntity(name);
    u64 childID = entity.GetUUID();
    u64 parentID = parent.GetUUID();
    // Fresh child: keep its local transform so it appears at the parent's
    // origin (world preservation would teleport it back to its old world spot,
    // i.e. the world origin for a new entity, cancelling the parent's).
    m_PendingReparents.push_back({childID, parentID, /*PreserveWorld=*/false});
    return entity;
}

void Scene::AttachChildEntity(Entity child, Entity parent)
{
    UHE_CORE_ASSERT(child, "Child is null!");
    UHE_CORE_ASSERT(parent, "Parent is null!");
    if (!child || !parent || child == parent)
        return;
    if (IsEntityParentOf(child, parent))
    {
        UHE_CORE_WARN("AttachChildEntity rejected (cycle): child={0} parent={1}", (u64)child.GetUUID(),
                      (u64)parent.GetUUID());
        return;
    }

    Entity currentParent = GetParentEntity(child);
    if (currentParent)
    {
        auto& siblings = currentParent.GetComponent<RelationshipComponent>().Children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), child.GetUUID()), siblings.end());
    }

    auto& rel = child.GetComponent<RelationshipComponent>();
    rel.Parent = parent.GetUUID();
    parent.GetComponent<RelationshipComponent>().Children.push_back(child.GetUUID());
    // Local transform intentionally untouched.
}

void Scene::DestroyEntity(Entity entity)
{
    std::unordered_set<u64> visited;
    auto destroyRecursive = [&](Entity e, auto& self) -> void {
        if (!e) return;
        u64 uuid = e.GetUUID();
        if (visited.find(uuid) != visited.end()) return;
        visited.insert(uuid);

        // Issue #17 lifetime fix: destroy the script instance (OnDestroy + delete)
        // BEFORE the registry handle dies, or it leaks and keeps a dangling Entity.
        if (e.HasComponent<NativeScriptComponent>())
        {
            auto& nsc = e.GetComponent<NativeScriptComponent>();
            if (nsc.Instance)
            {
                nsc.Instance->OnDestroy();
                if (nsc.DestroyScript)
                    nsc.DestroyScript(&nsc);
                else
                    delete nsc.Instance;
                nsc.Instance = nullptr;
            }
        }

        // Issue #17: recursively destroy children first.
        if (e.HasComponent<RelationshipComponent>())
        {
            auto childrenCopy = e.GetComponent<RelationshipComponent>().Children;
            for (u64 childID : childrenCopy)
            {
                Entity child = GetEntityWithUUID(childID);
                if (child)
                    self(child, self);
            }
        }

        // Detach from parent so the parent's child list stays valid.
        if (e.HasComponent<RelationshipComponent>())
        {
            u64 parentID = e.GetComponent<RelationshipComponent>().Parent;
            if (parentID != 0)
            {
                Entity parent = GetEntityWithUUID(parentID);
                if (parent)
                {
                    auto& parentChildren = parent.GetComponent<RelationshipComponent>().Children;
                    parentChildren.erase(std::remove(parentChildren.begin(), parentChildren.end(), e.GetUUID()),
                                         parentChildren.end());
                }
            }
        }

        UnindexEntity(uuid);
        m_registry.destroy(e);
    };

    destroyRecursive(entity, destroyRecursive);
}

// --- Issue #17: hierarchy helpers ---

Entity Scene::GetEntityWithUUID(u64 uuid)
{
    // O(1) index lookup; the old implementation scanned every IDComponent,
    // which dominated frame time once the hierarchy UI and rendering started
    // resolving UUIDs each frame.
    auto it = m_UUIDIndex.find(uuid);
    if (it != m_UUIDIndex.end() && m_registry.valid(it->second))
        return Entity{it->second, this};
    return {};
}

void Scene::IndexEntity(Entity entity, u64 uuid)
{
    if (!entity)
        return;
    m_UUIDIndex[uuid] = (entt::entity)entity;
}

void Scene::UnindexEntity(u64 uuid)
{
    m_UUIDIndex.erase(uuid);
}

bool Scene::IsEntityParentOf(Entity parent, Entity entity)
{
    if (!parent || !entity)
        return false;

    u64 targetID = entity.GetUUID();
    u64 cursorID = targetID;
    const int maxDepth = 4096; // cycle guard
    for (int i = 0; i < maxDepth; i++)
    {
        Entity cursor = GetEntityWithUUID(cursorID);
        if (!cursor || !cursor.HasComponent<RelationshipComponent>())
            return false;

        u64 parentID = cursor.GetComponent<RelationshipComponent>().Parent;
        if (parentID == 0)
            return false;
        if (parentID == parent.GetUUID())
            return true;
        cursorID = parentID;
    }
    return false;
}

Entity Scene::GetParentEntity(Entity entity)
{
    if (!entity || !entity.HasComponent<RelationshipComponent>())
        return {};
    u64 parentID = entity.GetComponent<RelationshipComponent>().Parent;
    if (parentID == 0)
        return {};
    return GetEntityWithUUID(parentID);
}

void Scene::ReparentEntity(Entity entity, Entity newParent)
{
    UHE_CORE_ASSERT(entity, "Entity is null!");
    UHE_CORE_ASSERT(newParent, "New parent is null!");
    if (!entity || !newParent)
        return;

    // Cannot parent under self or own descendant (would create a cycle).
    if (entity == newParent || IsEntityParentOf(entity, newParent))
    {
        UHE_CORE_WARN("ReparentEntity rejected (cycle): entity={0} newParent={1}", (u64)entity.GetUUID(),
                      (u64)newParent.GetUUID());
        return;
    }

    // Already a child of this parent: nothing to do. This also guards against
    // editor drag-drop firing twice per gesture (source + target both accept).
    Entity currentParent = GetParentEntity(entity);
    if (currentParent && currentParent == newParent)
        return;

    // Keep the entity's world transform while switching parents.
    glm::mat4 worldTransform = GetWorldSpaceTransformMatrix(entity);

    // Detach from the current parent.
    if (currentParent)
    {
        auto& siblings = currentParent.GetComponent<RelationshipComponent>().Children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), entity.GetUUID()), siblings.end());
    }

    // Attach to the new parent.
    auto& rel = entity.GetComponent<RelationshipComponent>();
    rel.Parent = newParent.GetUUID();
    newParent.GetComponent<RelationshipComponent>().Children.push_back(entity.GetUUID());

    SetLocalTransformFromWorld(entity, worldTransform);
}

void Scene::CollapseEntity(Entity entity)
{
    UHE_CORE_ASSERT(entity, "Entity is null!");
    if (!entity)
        return;

    // Issue #17 hardening: a stale child entry can make GetParentEntity
    // resolve to the entity itself (its own UUID listed under its Children).
    // Repair that state instead of detaching from itself.
    auto& rel = entity.GetComponent<RelationshipComponent>();
    auto& kids = rel.Children;
    kids.erase(std::remove(kids.begin(), kids.end(), entity.GetUUID()), kids.end());

    Entity parent = GetParentEntity(entity);
    if (!parent)
    {
        rel.Parent = 0; // already a root (or dangling parent reference)
        return;
    }
    if (parent == entity)
        return; // repaired above

    // Keep the entity's world transform, detach ONLY the entity: its children
    // stay attached and simply move along with it (the old implementation
    // re-parented every child to the grandparent, which scattered subtrees).
    glm::mat4 worldTransform = GetWorldSpaceTransformMatrix(entity);

    auto& siblings = parent.GetComponent<RelationshipComponent>().Children;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), entity.GetUUID()), siblings.end());

    rel.Parent = 0;

    SetLocalTransformFromWorld(entity, worldTransform);
}

void Scene::FlushPendingModelOps()
{
    if (!m_PendingModelExpands.empty())
    {
        auto pending = m_PendingModelExpands;
        m_PendingModelExpands.clear();
        for (u64 modelID : pending)
        {
            Entity model = GetEntityWithUUID(modelID);
            if (model)
                ExpandModelNodes(model);
        }
    }

    if (!m_PendingModelCollapses.empty())
    {
        auto pending = m_PendingModelCollapses;
        m_PendingModelCollapses.clear();
        for (u64 modelID : pending)
        {
            Entity model = GetEntityWithUUID(modelID);
            if (model)
                CollapseExpandedModel(model);
        }
    }
}

bool Scene::IsModelExpanded(Entity modelEntity)
{
    if (!modelEntity || !modelEntity.HasComponent<RelationshipComponent>())
        return false;

    auto childrenCopy = modelEntity.GetComponent<RelationshipComponent>().Children;
    for (u64 childID : childrenCopy)
    {
        Entity child = GetEntityWithUUID(childID);
        if (child && child.HasComponent<ModelNodeComponent>())
            return true;
    }
    return false;
}

void Scene::ExpandModelNodes(Entity modelEntity)
{
    UHE_CORE_ASSERT(modelEntity, "Model entity is null!");
    if (!modelEntity || !modelEntity.HasComponent<Model3DComponent>())
        return;

    auto& mc = modelEntity.GetComponent<Model3DComponent>();
    if (!mc.IsLoaded || !mc.ModelData)
        return;

    const auto& nodes = mc.ModelData->GetNodes();
    if (nodes.empty())
        return;

    // Idempotency guard: expanding twice used to duplicate the whole tree.
    if (IsModelExpanded(modelEntity))
    {
        UHE_CORE_WARN("ExpandModelNodes ignored: model is already expanded.");
        return;
    }

    u64 modelUUID = modelEntity.GetUUID();

    // Create one entity per glTF node.
    std::vector<Entity> nodeEntities(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++)
    {
        const auto& node = nodes[i];
        Entity nodeEntity = CreateEntity(node.Name.empty() ? "Node" : node.Name);

        // glTF node transforms are LOCAL to their parent node — assign them
        // directly, do NOT round-trip through ReparentEntity (which preserves
        // world transform and used to cancel out the model's own transform).
        auto& transform = nodeEntity.GetComponent<TransformComponent>();
        transform.Translation = node.Translation;
        transform.Rotation = glm::eulerAngles(node.Rotation);
        transform.Scale = node.Scale;

        auto& nodeComp = nodeEntity.AddComponent<ModelNodeComponent>();
        nodeComp.ModelEntity = modelUUID;
        nodeComp.NodeIndex = static_cast<int>(i);
        nodeComp.NodeName = node.Name;
        nodeComp.HasChildrenNodes = !node.Children.empty();

        nodeEntities[i] = nodeEntity;
    }

    // Wire the hierarchy directly (locals are already correct model-space).
    for (size_t i = 0; i < nodes.size(); i++)
    {
        const auto& node = nodes[i];
        auto& rel = nodeEntities[i].GetComponent<RelationshipComponent>();

        u64 parentUUID;
        if (node.Parent >= 0)
            parentUUID = nodeEntities[static_cast<size_t>(node.Parent)].GetUUID();
        else
            parentUUID = modelUUID; // glTF roots hang under the model entity

        rel.Parent = parentUUID;
        GetEntityWithUUID(parentUUID).GetComponent<RelationshipComponent>().Children.push_back(
            nodeEntities[i].GetUUID());
    }
}

void Scene::CollapseExpandedModel(Entity modelEntity)
{
    UHE_CORE_ASSERT(modelEntity, "Model entity is null!");
    if (!modelEntity || !IsModelExpanded(modelEntity))
        return;

    // Re-attach user-created entities that live inside the node tree back to
    // the model (world transform preserved), then destroy the node entities.
    // Recursive: user entities may sit anywhere in the glTF subtree.
    std::function<void(Entity)> foldSubtree = [&](Entity nodeEntity) {
        auto childrenCopy = nodeEntity.GetComponent<RelationshipComponent>().Children;
        for (u64 childID : childrenCopy)
        {
            Entity child = GetEntityWithUUID(childID);
            if (!child)
                continue;

            if (child.HasComponent<ModelNodeComponent>())
            {
                foldSubtree(child);
            }
            else
            {
                // User entity: move to the model with world transform kept.
                glm::mat4 world = GetWorldSpaceTransformMatrix(child);

                auto& childRel = child.GetComponent<RelationshipComponent>();
                auto& nodeChildren = nodeEntity.GetComponent<RelationshipComponent>().Children;
                nodeChildren.erase(std::remove(nodeChildren.begin(), nodeChildren.end(), childID), nodeChildren.end());

                childRel.Parent = modelEntity.GetUUID();
                modelEntity.GetComponent<RelationshipComponent>().Children.push_back(childID);

                SetLocalTransformFromWorld(child, world);
            }
        }
    };

    auto modelChildren = modelEntity.GetComponent<RelationshipComponent>().Children;
    for (u64 childID : modelChildren)
    {
        Entity child = GetEntityWithUUID(childID);
        if (child && child.HasComponent<ModelNodeComponent>())
        {
            foldSubtree(child);
            DestroyEntity(child); // removes any remaining node subtree
        }
    }
}

std::vector<Entity> Scene::GetRootEntities()
{
    std::vector<Entity> roots;

    // A root is an entity whose Parent is 0 or dangling. Stale child-list
    // entries elsewhere do NOT disqualify it (Parent is authoritative);
    // consumers validate child edges against RelationshipComponent.Parent
    // instead, so a stale entry can never draw or serialize a duplicate.
    auto view = m_registry.view<RelationshipComponent>();
    for (auto handle : view)
    {
        auto& rel = view.get<RelationshipComponent>(handle);
        bool danglingParent = rel.Parent != 0 && !GetEntityWithUUID(rel.Parent);
        if (rel.Parent == 0 || danglingParent)
            roots.push_back(Entity{handle, this});
    }
    return roots;
}

glm::mat4 Scene::GetWorldTransformByUUID(u64 uuid)
{
    auto it = m_WorldTransformByUUID.find(uuid);
    if (it != m_WorldTransformByUUID.end())
        return it->second;

    // Fall back to walking the chain (cache not built or entity created this
    // frame). Guard against stale UUIDs that no longer resolve.
    Entity entity = GetEntityWithUUID(uuid);
    if (!entity)
        return glm::mat4(1.0f);
    return GetWorldSpaceTransformMatrix(entity);
}

glm::mat4 Scene::GetWorldSpaceTransformMatrix(Entity entity)
{
    glm::mat4 world{1.0f};

    // Walk the parent chain to the root and multiply transforms back down.
    std::vector<u64> chain;
    Entity current = entity;
    const int maxDepth = 4096; // cycle guard
    for (int i = 0; i < maxDepth && current; i++)
    {
        chain.push_back(current.GetUUID());
        current = GetParentEntity(current);
    }

    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        Entity node = GetEntityWithUUID(*it);
        if (!node || !node.HasComponent<TransformComponent>())
            continue;
        world = world * node.GetComponent<TransformComponent>().GetTransform();
    }

    return world;
}

void Scene::SetLocalTransformFromWorld(Entity entity, const glm::mat4& worldTransform)
{
    if (!entity || !entity.HasComponent<TransformComponent>())
        return;

    Entity parent = GetParentEntity(entity);
    if (parent)
    {
        glm::mat4 parentWorld = GetWorldSpaceTransformMatrix(parent);
        glm::mat4 local = glm::inverse(parentWorld) * worldTransform;

        glm::vec3 translation, scale, skew;
        glm::quat rotation;
        glm::vec4 perspective;
        if (glm::decompose(local, scale, rotation, translation, skew, perspective))
        {
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = translation;
            transform.Rotation = glm::eulerAngles(rotation);
            transform.Scale = scale;
        }
    }
    else
    {
        glm::vec3 translation, scale, skew;
        glm::quat rotation;
        glm::vec4 perspective;
        if (glm::decompose(worldTransform, scale, rotation, translation, skew, perspective))
        {
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = translation;
            transform.Rotation = glm::eulerAngles(rotation);
            transform.Scale = scale;
        }
    }
}

void Scene::OnViewportResize(u32 width, u32 height)
{
    m_ViewportWidth = width;
    m_ViewportHeight = height;

    auto view = m_registry.view<CameraComponent>();
    for (auto entity : view)
    {
        auto& cameraComponent = view.get<CameraComponent>(entity);
        if (!cameraComponent.FixedAspectRatio)
        {
            cameraComponent.Camera.SetViewportSize(width, height);
        }
    }
}

void Scene::OnUpdateEditor(Timestep ts, EditorCamera& camera)
{
    // Issue #17 hardening: apply hierarchy mutations deferred from the editor
    // UI (component pools are only touched here, outside any registry view
    // iteration, so no entt iterator gets invalidated).
    FlushPendingReparents();
    FlushPendingCollapses();
    FlushPendingModelOps();

    m_registry.view<NativeScriptComponent>().each(
        [=, this](auto entity, auto& nsc)
        {
            if (!nsc.Instance)
            {
                nsc.Instance = nsc.InstantiateScript();
                nsc.Instance->m_Entity = Entity{entity, this};
                nsc.Instance->OnCreate();
            }
            nsc.Instance->OnUpdate(ts);
        });

    {
        auto view = m_registry.view<AnimatorComponent>();
        for (auto entity : view)
        {
            auto& anim = view.get<AnimatorComponent>(entity);
            if (anim.Animator)
            {
                // Issue #41: mirror the component's playback settings onto
                // the runtime so editor/serializer changes apply immediately.
                anim.Animator->SetLoopMode(anim.LoopMode);
                anim.Animator->SetReversed(anim.Reverse);
                anim.Animator->SetRootMotionEnabled(anim.RootMotion);
                if (anim.RootMotion && m_registry.all_of<TransformComponent>(entity))
                {
                    // Root motion: hand the extracted delta to the entity.
                    auto& tc = m_registry.get<TransformComponent>(entity);
                    tc.Translation += anim.Animator->GetRootMotionDelta();
                }
                if (anim.IsPlaying)
                {
                    anim.Animator->UpdateAnimation(ts * anim.PlaybackSpeed);
                }
            }
        }
    }

    // Issue #17: refresh world transforms from the hierarchy before rendering.
    UpdateWorldTransformCache();

    Renderer2D::BeginScene(camera);
    RenderSprites(ts, m_WorldTransformCache);
    RenderLightIcons(camera, m_WorldTransformCache);
    Renderer2D::EndScene();

    auto lights = RD3d::LightSystem::ExtractLights(m_registry);
    Renderer3D::BeginScene(camera, lights);
    Renderer3D::DrawGrid();
    RenderModels(ts, m_WorldTransformCache);
    Renderer3D::EndScene();
}

void Scene::FlushPendingReparents()
{
    if (m_PendingReparents.empty())
        return;

    // Copy: the vector can be appended to while we work (nested creations).
    auto pending = m_PendingReparents;
    m_PendingReparents.clear();

    for (auto& [childID, parentID, preserveWorld] : pending)
    {
        Entity child = GetEntityWithUUID(childID);
        Entity parent = GetEntityWithUUID(parentID);
        if (child && parent)
        {
            if (preserveWorld)
                ReparentEntity(child, parent);
            else
                AttachChildEntity(child, parent);
        }
    }
}

void Scene::FlushPendingCollapses()
{
    if (m_PendingCollapses.empty())
        return;

    auto pending = m_PendingCollapses;
    m_PendingCollapses.clear();

    for (u64 entityID : pending)
    {
        Entity entity = GetEntityWithUUID(entityID);
        if (entity)
            CollapseEntity(entity);
    }
}

static glm::mat4 GetWorldFromCache(const std::unordered_map<entt::entity, glm::mat4>& cache, entt::entity entity)
{
    auto it = cache.find(entity);
    return it != cache.end() ? it->second : glm::mat4(1.0f);
}

void Scene::UpdateWorldTransformCache()
{
    m_WorldTransformCache.clear();
    m_WorldTransformByUUID.clear();

    // Bucket entities by parent UUID in one pass; entities without a parent
    // (or whose parent no longer exists) become roots.
    std::unordered_set<u64> allIDs;
    auto idView = m_registry.view<IDComponent>();
    for (auto handle : idView)
        allIDs.insert(idView.get<IDComponent>(handle).ID);

    std::unordered_map<u64, std::vector<Entity>> childrenOf;
    std::vector<Entity> roots;

    auto transformView = m_registry.view<TransformComponent>();
    for (auto handle : transformView)
    {
        Entity entity{handle, this};
        u64 parentID = 0;
        if (entity.HasComponent<RelationshipComponent>())
            parentID = entity.GetComponent<RelationshipComponent>().Parent;

        if (parentID != 0 && allIDs.count(parentID))
            childrenOf[parentID].push_back(entity);
        else
            roots.push_back(entity);
    }

    // Iterative DFS so deep glTF hierarchies cannot blow the call stack.
    struct StackEntry
    {
        Entity entity;
        glm::mat4 parentWorld;
    };
    std::vector<StackEntry> stack;
    stack.reserve(roots.size());
    for (auto it = roots.rbegin(); it != roots.rend(); ++it)
        stack.push_back({*it, glm::mat4(1.0f)});

    while (!stack.empty())
    {
        StackEntry entry = stack.back();
        stack.pop_back();

        glm::mat4 world = entry.parentWorld;
        if (entry.entity.HasComponent<TransformComponent>())
            world = entry.parentWorld * entry.entity.GetComponent<TransformComponent>().GetTransform();

        m_WorldTransformCache[(entt::entity)entry.entity] = world;
        m_WorldTransformByUUID[entry.entity.GetUUID()] = world;

        auto childIt = childrenOf.find(entry.entity.GetUUID());
        if (childIt != childrenOf.end())
        {
            for (auto cit = childIt->second.rbegin(); cit != childIt->second.rend(); ++cit)
                stack.push_back({*cit, world});
        }
    }
}

void Scene::RenderSprites(Timestep ts, const std::unordered_map<entt::entity, glm::mat4>& worldTransforms)
{
    auto animView = m_registry.view<SpriteAnimationComponent>();
    for (auto entity : animView)
    {
        auto& comp = animView.get<SpriteAnimationComponent>(entity);
        comp.Animation.Tick(ts);
    }

    auto spriteView = m_registry.view<TransformComponent, SpriteRendererComponent>();
    for (auto entityID : spriteView)
    {
        auto [transform, sprite] = spriteView.get<TransformComponent, SpriteRendererComponent>(entityID);

        Renderer2D::DrawSprite(GetWorldFromCache(worldTransforms, entityID), sprite, (i32)entityID);
    }

    auto textView = m_registry.view<TransformComponent, TextComponent>();
    for (auto entityID : textView)
    {
        auto [transform, text] = textView.get<TransformComponent, TextComponent>(entityID);
        Renderer2D::DrawString(text.TextString, text.FontAsset, GetWorldFromCache(worldTransforms, entityID), text.Color,
                               text.Kerning, text.LineSpacing, (i32)entityID);
    }

    auto animOnlyView = m_registry.view<TransformComponent, SpriteAnimationComponent>();
    for (auto entityID : animOnlyView)
    {
        if (m_registry.all_of<SpriteRendererComponent>(entityID))
            continue;

        auto [transform, comp] = animOnlyView.get<TransformComponent, SpriteAnimationComponent>(entityID);

        Ref<SubTexture2D> overrideSubTex = comp.Animation.GetCurrentFrame();
        if (overrideSubTex)
        {
            Renderer2D::DrawQuad(GetWorldFromCache(worldTransforms, entityID), overrideSubTex, 1.0f, comp.Color);
        }
    }
}

void Scene::RenderLightIcons(EditorCamera& camera, const std::unordered_map<entt::entity, glm::mat4>& worldTransforms)
{
    if (!GetShowLightIcons()) return;

    if (!m_DirLightIcon)
    {
        std::string dirPath = (FileSystem::Get().GetRootPath() / "assets/icon/directionLight.png").string();
        m_DirLightIcon = Texture2D::Create(dirPath);
    }
    if (!m_PointLightIcon)
    {
        std::string pointPath = (FileSystem::Get().GetRootPath() / "assets/icon/pointLight.png").string();
        m_PointLightIcon = Texture2D::Create(pointPath);
    }

    auto rotation = glm::mat4(camera.GetOrientation());

    auto dirLightView = m_registry.view<TransformComponent, DirectionalLightComponent>();
    for (auto entity : dirLightView)
    {
        auto [transform, light] = dirLightView.get<TransformComponent, DirectionalLightComponent>(entity);
        glm::mat4 billTransform =
            GetWorldFromCache(worldTransforms, entity) * rotation * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
        Renderer2D::DrawQuad(billTransform, m_DirLightIcon, 1.0f, glm::vec4(1.0f), (int)entity);
    }

    auto pointLightView = m_registry.view<TransformComponent, PointLightComponent>();
    for (auto entity : pointLightView)
    {
        auto [transform, light] = pointLightView.get<TransformComponent, PointLightComponent>(entity);
        glm::mat4 billTransform =
            GetWorldFromCache(worldTransforms, entity) * rotation * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
        Renderer2D::DrawQuad(billTransform, m_PointLightIcon, 1.0f, glm::vec4(1.0f), (int)entity);
    }
}

void Scene::RenderModels(Timestep ts, const std::unordered_map<entt::entity, glm::mat4>& worldTransforms)
{
    auto view = m_registry.view<TransformComponent, Model3DComponent>();
    for (auto entity : view)
    {
        auto [transform, model] = view.get<TransformComponent, Model3DComponent>(entity);
        if (!model.IsLoaded || !model.ModelData)
            continue;

        // Issue #17: if this model's glTF tree was expanded into child node
        // entities, the children draw themselves; the parent renders nothing.
        bool hasNodeChildren = false;
        if (m_registry.all_of<RelationshipComponent>(entity))
        {
            for (u64 childID : m_registry.get<RelationshipComponent>(entity).Children)
            {
                Entity child = GetEntityWithUUID(childID);
                if (child && m_registry.all_of<ModelNodeComponent>(child))
                {
                    hasNodeChildren = true;
                    break;
                }
            }
        }
        if (hasNodeChildren)
            continue;

        const RD3d::Animator* animator = nullptr;
        if (m_registry.all_of<AnimatorComponent>(entity))
        {
            auto& animComp = m_registry.get<AnimatorComponent>(entity);
            if (animComp.Animator)
                animator = animComp.Animator.get();
        }
        Renderer3D::SubmitModel(*model.ModelData, GetWorldFromCache(worldTransforms, entity), (int)entity, animator);
    }

    // Issue #17: leaf nodes of expanded glTF trees submit their sub-mesh with
    // the accumulated world transform so every part is individually placed.
    // NOTE: group nodes with their own mesh are drawn here too — only nodes
    // without a mesh are skipped.
    // One bone upload per expanded model: all node meshes of a model share the
    // binding (a naive per-node upload would re-upload the same matrices once
    // per node and break the bone-offset contract).
    auto nodeView = m_registry.view<ModelNodeComponent>();

    std::unordered_map<u64, Renderer3D::BoneBinding> boneCache;

    for (auto entity : nodeView)
    {
        auto& nodeComp = nodeView.get<ModelNodeComponent>(entity);

        Entity modelEntity = GetEntityWithUUID(nodeComp.ModelEntity);
        if (!modelEntity || !modelEntity.HasComponent<Model3DComponent>())
            continue;

        auto& mc = modelEntity.GetComponent<Model3DComponent>();
        if (!mc.IsLoaded || !mc.ModelData)
            continue;

        const auto& nodes = mc.ModelData->GetNodes();
        if (nodeComp.NodeIndex < 0 || nodeComp.NodeIndex >= static_cast<int>(nodes.size()))
            continue;
        int meshIndex = nodes[nodeComp.NodeIndex].MeshIndex;
        if (meshIndex < 0)
            continue;

        const auto& meshes = mc.ModelData->GetMesh();
        if (meshIndex >= static_cast<int>(meshes.size()))
            continue;

        // Use cached binding or prepare it once per model entity
        auto cacheIt = boneCache.find(nodeComp.ModelEntity);
        if (cacheIt == boneCache.end())
        {
            const RD3d::Animator* animator = nullptr;
            if (modelEntity.HasComponent<AnimatorComponent>())
            {
                auto& animComp = modelEntity.GetComponent<AnimatorComponent>();
                if (animComp.Animator)
                    animator = animComp.Animator.get();
            }
            boneCache[nodeComp.ModelEntity] = Renderer3D::PrepareBoneBinding(animator);
            cacheIt = boneCache.find(nodeComp.ModelEntity);
        }

        Renderer3D::SubmitMesh(meshes[meshIndex], GetWorldFromCache(worldTransforms, entity), (int)entity,
                               mc.ModelData->GetMaterials(), cacheIt->second.BufferIndex, cacheIt->second.Offset);
    }
}

void Scene::OnUpdateRuntime(Timestep ts)
{
    // Issue #17 hardening: apply hierarchy mutations deferred from the editor
    // UI before anything reads the tree this frame.
    FlushPendingReparents();
    FlushPendingCollapses();
    FlushPendingModelOps();

    {
        auto view = m_registry.view<AnimatorComponent>();
        for (auto entity : view)
        {
            auto& anim = view.get<AnimatorComponent>(entity);
            if (anim.Animator)
            {
                // Issue #41: mirror the component's playback settings onto
                // the runtime so editor/serializer changes apply immediately.
                anim.Animator->SetLoopMode(anim.LoopMode);
                anim.Animator->SetReversed(anim.Reverse);
                anim.Animator->SetRootMotionEnabled(anim.RootMotion);
                if (anim.RootMotion && m_registry.all_of<TransformComponent>(entity))
                {
                    // Root motion: hand the extracted delta to the entity.
                    auto& tc = m_registry.get<TransformComponent>(entity);
                    tc.Translation += anim.Animator->GetRootMotionDelta();
                }
                if (anim.IsPlaying)
                {
                    anim.Animator->UpdateAnimation(ts * anim.PlaybackSpeed);
                }
            }
        }
    }

    {
        m_registry.view<NativeScriptComponent>().each(
            [=, this](auto entity, auto& nsc)
            {
                if (!nsc.Instance)
                {
                    nsc.Instance = nsc.InstantiateScript();
                    nsc.Instance->m_Entity = Entity{entity, this};
                    nsc.Instance->OnCreate();
                }
                nsc.Instance->OnUpdate(ts);
            });
    }

    // Physics
    {
        const int subStepCount = 4;
        b2World_Step(m_PhysicsWorldId, ts, subStepCount);

        // Retrieve transforms from Box2D
        auto view = m_registry.view<TransformComponent, RigidBody2DComponent>();
        for (auto entity : view)
        {
            auto [transform, rb2d] = view.get<TransformComponent, RigidBody2DComponent>(entity);

            if (!b2Body_IsValid(rb2d.RuntimeBody))
                continue;

            b2Vec2 position = b2Body_GetPosition(rb2d.RuntimeBody);
            transform.Translation.x = position.x;
            transform.Translation.y = position.y;

            b2Rot rotation = b2Body_GetRotation(rb2d.RuntimeBody);
            transform.Rotation.z = b2Rot_GetAngle(rotation);
        }

        // 3D Physics Jolt
        m_PhysicsSystem3D.Update(ts);
        JPH::BodyInterface* bodyInterface = m_PhysicsSystem3D.GetBodyInterface();

        auto view3d = m_registry.view<TransformComponent, RigidBody3DComponent>();
        for (auto entity : view3d)
        {
            auto [transform, rb3d] = view3d.get<TransformComponent, RigidBody3DComponent>(entity);

            if (rb3d.RuntimeBodyID == 0xFFFFFFFF)
                continue;

            JPH::BodyID bodyID(rb3d.RuntimeBodyID);
            if (!bodyInterface->IsActive(bodyID))
                continue;

            JPH::Vec3 position = bodyInterface->GetPosition(bodyID);
            JPH::Quat rotation = bodyInterface->GetRotation(bodyID);

            transform.Translation = glm::vec3(position.GetX(), position.GetY(), position.GetZ());
            // glm::quat constructor is (w, x, y, z)
            transform.Rotation = glm::eulerAngles(glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()));
        }
    }

    // Issue #17: refresh world transforms after scripts/physics so rendering
    // uses this frame's final transforms.
    UpdateWorldTransformCache();

    Camera* mainCamera = nullptr;
    glm::mat4 cameraTransform;

    auto view = m_registry.view<TransformComponent, CameraComponent>();
    for (auto entity : view)
    {
        auto [transform, camera] = view.get<TransformComponent, CameraComponent>(entity);

        if (camera.Primary)
        {
            mainCamera = &camera.Camera;
            // Issue #17: the camera participates in the hierarchy too.
            cameraTransform = GetWorldFromCache(m_WorldTransformCache, entity);
            break;
        }
    }

    if (mainCamera)
    {
        Renderer2D::BeginScene(*mainCamera, cameraTransform);
        RenderSprites(ts, m_WorldTransformCache);
        Renderer2D::EndScene();
        
        auto lights = RD3d::LightSystem::ExtractLights(m_registry);
        Renderer3D::BeginScene(*mainCamera, cameraTransform, lights);

        RenderModels(ts, m_WorldTransformCache);
        Renderer3D::EndScene();
    }
}

void Scene::OnRuntimeStart()
{
    // 1. Define and Create the World
    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = {0.0f, -9.8f};
    m_PhysicsWorldId = b2CreateWorld(&worldDef);

    auto view = m_registry.view<TransformComponent, RigidBody2DComponent, BoxColliderComponent>();

    for (auto entity : view)
    {
        auto [transform, rb2d, bc2d] = view.get<TransformComponent, RigidBody2DComponent, BoxColliderComponent>(entity);

        // --- Body Configuration ---
        b2BodyDef bodyDef = b2DefaultBodyDef();

        switch (rb2d.Type)
        {
            case RigidBody2DComponent::BodyType::Static:
                bodyDef.type = b2_staticBody;
                break;
            case RigidBody2DComponent::BodyType::Dynamic:
                bodyDef.type = b2_dynamicBody;
                break;
            case RigidBody2DComponent::BodyType::Kinematic:
                bodyDef.type = b2_kinematicBody;
                break;
        }

        bodyDef.position = {transform.Translation.x, transform.Translation.y};
        // Ensure Rotation.z is in Radians!
        bodyDef.rotation = b2MakeRot(transform.Rotation.z);
        bodyDef.motionLocks.angularZ = rb2d.FixedRotation;

        // Create the body
        b2BodyId bodyId = b2CreateBody(m_PhysicsWorldId, &bodyDef);

        // FIX: Store the whole struct, not just the index
        rb2d.RuntimeBody = bodyId;

        // --- Shape/Fixture Configuration ---
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.density = bc2d.Density;
        // Properties moved into the material sub-struct
        shapeDef.material.friction = bc2d.Friction;
        shapeDef.material.restitution = bc2d.Restitution;

        // Box2D v3 uses half-extents (width/2, height/2), scaled by transform
        b2Polygon box = b2MakeOffsetBox(bc2d.Size.x * transform.Scale.x * 0.5f, bc2d.Size.y * transform.Scale.y * 0.5f,
                                        {bc2d.Offset.x, bc2d.Offset.y},
                                        b2MakeRot(0.0f) // The rotation of the box itself relative to body
        );

        b2CreatePolygonShape(bodyId, &shapeDef, &box);
    }

    // --- 3D Physics (Jolt) ---
    m_PhysicsSystem3D.InitializeScene();
    JPH::BodyInterface* bodyInterface = m_PhysicsSystem3D.GetBodyInterface();

    auto view3d = m_registry.view<TransformComponent, RigidBody3DComponent>();
    for (auto entity : view3d)
    {
        auto [transform, rb3d] = view3d.get<TransformComponent, RigidBody3DComponent>(entity);

        JPH::ShapeRefC shape = nullptr;
        float friction = 0.2f;
        float restitution = 0.0f;

        // Determine shape
        if (m_registry.all_of<BoxCollider3DComponent>(entity))
        {
            auto& bc = m_registry.get<BoxCollider3DComponent>(entity);
            glm::vec3 scaleExtents = bc.HalfExtent * transform.Scale;
            shape = new JPH::BoxShape(JPH::Vec3(scaleExtents.x, scaleExtents.y, scaleExtents.z));
            friction = bc.Friction;
            restitution = bc.Restitution;
        }
        else if (m_registry.all_of<SphereCollider3DComponent>(entity))
        {
            auto& sc = m_registry.get<SphereCollider3DComponent>(entity);
            float maxScale = std::max(std::max(transform.Scale.x, transform.Scale.y), transform.Scale.z);
            shape = new JPH::SphereShape(sc.Radius * maxScale);
            friction = sc.Friction;
            restitution = sc.Restitution;
        }
        else if (m_registry.all_of<CapsuleCollider3DComponent>(entity))
        {
            auto& cc = m_registry.get<CapsuleCollider3DComponent>(entity);
            float maxScale = std::max(transform.Scale.x, transform.Scale.z);
            shape = new JPH::CapsuleShape(cc.HalfHeight * transform.Scale.y, cc.Radius * maxScale);
            friction = cc.Friction;
            restitution = cc.Restitution;
        }

        if (shape)
        {
            JPH::EMotionType motionType = JPH::EMotionType::Static;
            JPH::ObjectLayer layer = 0; // Layers::NON_MOVING

            if (rb3d.Type == RigidBody3DComponent::BodyType::Dynamic)
            {
                motionType = JPH::EMotionType::Dynamic;
                layer = 1; // Layers::MOVING
            }
            else if (rb3d.Type == RigidBody3DComponent::BodyType::Kinematic)
            {
                motionType = JPH::EMotionType::Kinematic;
                layer = 1; // Layers::MOVING
            }

            glm::quat q = glm::quat(transform.Rotation);
            JPH::BodyCreationSettings bodySettings(
                shape, 
                JPH::Vec3(transform.Translation.x, transform.Translation.y, transform.Translation.z),
                JPH::Quat(q.x, q.y, q.z, q.w), 
                motionType, 
                layer
            );

            bodySettings.mFriction = friction;
            bodySettings.mRestitution = restitution;
            bodySettings.mLinearDamping = rb3d.LinearDamping;
            bodySettings.mAngularDamping = rb3d.AngularDamping;
            bodySettings.mIsSensor = rb3d.IsSensor;
            
            if (rb3d.Type == RigidBody3DComponent::BodyType::Dynamic)
            {
                bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                bodySettings.mMassPropertiesOverride.mMass = rb3d.Mass;
            }

            JPH::Body* body = bodyInterface->CreateBody(bodySettings);
            if (body)
            {
                bodyInterface->AddBody(body->GetID(), JPH::EActivation::Activate);
                rb3d.RuntimeBodyID = body->GetID().GetIndexAndSequenceNumber();
            }
        }
    }
}

void Scene::OnRuntimeStop()
{
    // Issue #17 lifetime fix: the runtime (play-mode) scene copy instantiated
    // its own script instances; release them before the copy is swapped out,
    // otherwise every play session leaked the whole instance set.
    DestroyScriptInstances();

    if (b2World_IsValid(m_PhysicsWorldId))
    {
        b2DestroyWorld(m_PhysicsWorldId);
        m_PhysicsWorldId = b2_nullWorldId;
    }

    // 3D Physics
    m_PhysicsSystem3D.ShutdownScene();
}

Entity Scene::GetPrimaryCameraEntity()
{
    auto view = m_registry.view<TransformComponent, CameraComponent>();
    for (auto entity : view)
    {
        const auto& camera = view.get<CameraComponent>(entity);
        if (camera.Primary)
            return Entity{entity, this};
    }
    return {};
}

template <typename T> void Scene::OnComponentAdded(Entity entity, T& component)
{
    static_assert(sizeof(T) == 0, "Only specialized components can be added!");
}

template <> void Scene::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent& component) {}
template <> void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent& component)
{
    component.Camera.SetViewportSize(m_ViewportWidth, m_ViewportHeight);
}

template <> void Scene::OnComponentAdded<TagComponent>(Entity entity, TagComponent& component) {}
template <> void Scene::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent& component) {}
template <> void Scene::OnComponentAdded<TextComponent>(Entity entity, TextComponent& component) {}

template <> void Scene::OnComponentAdded<SpriteAnimationComponent>(Entity entity, SpriteAnimationComponent& component)
{
}
template <> void Scene::OnComponentAdded<NativeScriptComponent>(Entity entity, NativeScriptComponent& component) {}
template <> void Scene::OnComponentAdded<RigidBody2DComponent>(Entity entity, RigidBody2DComponent& component) {}
template <> void Scene::OnComponentAdded<BoxColliderComponent>(Entity entity, BoxColliderComponent& component) {}
template <> void Scene::OnComponentAdded<IDComponent>(Entity entity, IDComponent& component) {};
template <> void Scene::OnComponentAdded<Model3DComponent>(Entity entity, Model3DComponent& component) {};
template <> void Scene::OnComponentAdded<DirectionalLightComponent>(Entity entity, DirectionalLightComponent& component) {};
template <> void Scene::OnComponentAdded<PointLightComponent>(Entity entity, PointLightComponent& component) {};
template <> void Scene::OnComponentAdded<AnimatorComponent>(Entity entity, AnimatorComponent& component) {};
template <> void Scene::OnComponentAdded<ModelNodeComponent>(Entity entity, ModelNodeComponent& component) {};
template <> void Scene::OnComponentAdded<RelationshipComponent>(Entity entity, RelationshipComponent& component) {};
template <> void Scene::OnComponentAdded<RigidBody3DComponent>(Entity entity, RigidBody3DComponent& component) {};
template <> void Scene::OnComponentAdded<BoxCollider3DComponent>(Entity entity, BoxCollider3DComponent& component) {};
template <> void Scene::OnComponentAdded<SphereCollider3DComponent>(Entity entity, SphereCollider3DComponent& component) {};
template <> void Scene::OnComponentAdded<CapsuleCollider3DComponent>(Entity entity, CapsuleCollider3DComponent& component) {};

} // namespace UHE
