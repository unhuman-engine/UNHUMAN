#pragma once

#include <unordered_map>
#include <unordered_set>
#include <box2d/types.h>
#include <example/stb_image.h>
#include "UHE/Core/Timestep.h"
#include "UHE/Renderer/EditorCamera.h"
#include "UHE/Scene/Components.h"
#include "UHE/Physics/PhysicsSystem3D.h"
#include "entt.hpp"
class b2World;

namespace UHE
{

class Entity;
struct TransformComponent;
struct CameraComponent;
struct TagComponent;
struct SpriteRendererComponent;
struct SpriteAnimationComponent;
struct NativeScriptComponent;
struct RigidBody2DComponent;
struct BoxColliderComponent;
struct IDComponent;
struct Model3DComponent;
struct ModelNodeComponent;
struct RelationshipComponent;
struct RigidBody3DComponent;
struct BoxCollider3DComponent;
struct SphereCollider3DComponent;
struct CapsuleCollider3DComponent;

class UHE_API Scene
{
public:
    Scene();
    ~Scene();

    Entity CreateEntity(const std::string& name = std::string());
    // Issue #17: create an entity parented under 'parent' (world-space
    // transform is preserved).
    Entity CreateChildEntity(Entity parent, const std::string& name = std::string());

    void DestroyEntity(Entity entity);

    // Issue #17: hierarchy helpers operating on RelationshipComponent.
    Entity GetEntityWithUUID(u64 uuid);
    // True when 'entity' is a strict descendant of 'parent' (self returns false).
    bool IsEntityParentOf(Entity parent, Entity entity);
    Entity GetParentEntity(Entity entity);
    void ReparentEntity(Entity entity, Entity newParent);
    // Detach 'entity' from its parent (making it a root) while keeping its
    // world transform; its children stay attached and move along with it.
    void CollapseEntity(Entity entity);

    // Issue #17: expand a model's glTF node tree into child entities so every
    // node is individually controllable in the editor. Idempotent: a model
    // that is already expanded is left untouched.
    void ExpandModelNodes(Entity modelEntity);
    // Fold an expanded model back into a single entity: glTF node entities are
    // removed; user-created entities under them are re-attached to the model
    // with their world transforms preserved.
    void CollapseExpandedModel(Entity modelEntity);
    // True when 'modelEntity' currently has expanded glTF node entities.
    bool IsModelExpanded(Entity modelEntity);

    // Issue #17 hardening: model expand/collapse requested from the editor UI
    // is queued and applied at a safe point in the frame.
    void QueueModelExpand(u64 modelEntityID) { if (modelEntityID) m_PendingModelExpands.push_back(modelEntityID); }
    void QueueModelCollapse(u64 modelEntityID) { if (modelEntityID) m_PendingModelCollapses.push_back(modelEntityID); }

    // Definitive root list: every entity whose Parent is 0 or dangling.
    // Deduplicates against the Children lists, so UI/serialization must use
    // this instead of iterating the registry (prevents ghost duplicates when
    // a stale child entry exists on two parents).
    std::vector<Entity> GetRootEntities();

    // Issue #17 hardening: hierarchy mutations queued from the editor UI and
    // applied at a safe point in the frame (start of the next update).
    void QueueReparent(u64 entityID, u64 newParentID)
    {
        m_PendingReparents.push_back({entityID, newParentID, true});
    }
    void QueueCollapse(u64 entityID)
    {
        if (entityID != 0)
            m_PendingCollapses.push_back(entityID);
    }

    // World-space transform matrix accumulated over the parent chain.
    glm::mat4 GetWorldSpaceTransformMatrix(Entity entity);
    // Cached variant (fresh only after UpdateWorldTransformCache ran this
    // frame); falls back to walking the chain. Safe to call while iterating.
    glm::mat4 GetWorldTransformByUUID(u64 uuid);
    // Local transform of 'entity' that yields 'worldTransform' under its
    // current parent chain.
    void SetLocalTransformFromWorld(Entity entity, const glm::mat4& worldTransform);

    void OnViewportResize(u32 width, u32 height);

    void OnUpdateEditor(Timestep ts, EditorCamera& camera);
    void OnUpdateRuntime(Timestep ts);

    void OnRuntimeStart();
    void OnRuntimeStop();

    Entity GetPrimaryCameraEntity();

    static Ref<Scene> Copy(Ref<Scene> other);
    entt::registry& GetRegistry() { return m_registry; }

    static bool& GetShowLightIcons() { static bool s_ShowLightIcons = true; return s_ShowLightIcons; }

public:
    template <typename T> void OnComponentAdded(Entity entity, T& components);

private:
    // Issue #17 lifetime fix: NativeScriptComponent::Instance is heap-owned
    // (created by InstantiateScript) and must be released via OnDestroy() +
    // DestroyScript on entity deletion AND scene teardown, or every script
    // leaks and keeps a dangling Entity handle.
    void DestroyScriptInstances();

    // Issue #17 hardening: hierarchy mutations requested from the editor UI
    // while it iterates the registry are queued here and applied at a safe
    // point in the frame (start of OnUpdateEditor / OnUpdateRuntime).
    void FlushPendingReparents();
    void FlushPendingCollapses();
    void FlushPendingModelOps();
    // Attach 'child' under 'parent' WITHOUT preserving its world transform:
    // the local transform is kept as-is (new entities appear at the parent's
    // origin). ReparentEntity is the world-preserving variant.
    void AttachChildEntity(Entity child, Entity parent);
    struct PendingReparent
    {
        u64 ChildID;
        u64 ParentID;
        bool PreserveWorld; // true = drag-drop re-parent, false = fresh child
    };
    std::vector<PendingReparent> m_PendingReparents;
    std::vector<u64> m_PendingCollapses;
    std::vector<u64> m_PendingModelExpands;
    std::vector<u64> m_PendingModelCollapses;

    // Issue #17: cached world-space transforms for every entity, built once
    // per frame from the RelationshipComponent tree.
    void UpdateWorldTransformCache();

    void RenderSprites(Timestep ts, const std::unordered_map<entt::entity, glm::mat4>& worldTransforms);
    void RenderModels(Timestep ts, const std::unordered_map<entt::entity, glm::mat4>& worldTransforms);
    void RenderLightIcons(EditorCamera& camera, const std::unordered_map<entt::entity, glm::mat4>& worldTransforms);

    entt::registry m_registry;
    u32 m_ViewportWidth = 0, m_ViewportHeight = 0;

    // Issue #17: world-space transform cache, rebuilt each frame.
    std::unordered_map<entt::entity, glm::mat4> m_WorldTransformCache;
    // UUID-keyed view of the same cache; used by the editor UI (stable across
    // entity destruction during iteration).
    std::unordered_map<u64, glm::mat4> m_WorldTransformByUUID;

    // UUID index maintenance (friends SceneSerializer/Entity also rely on it
    // via Copy/Deserialize restoring IDs).
    void IndexEntity(Entity entity, u64 uuid);
    void UnindexEntity(u64 uuid);

    // Issue #17 hardening: UUID -> entity index so hierarchy lookups are O(1)
    // instead of a full IDComponent scan per call (the old scan ran inside
    // tree iteration, rendering, and every reparent).
    std::unordered_map<u64, entt::entity> m_UUIDIndex;

    b2WorldId m_PhysicsWorldId = b2_nullWorldId;
    Physics::PhysicsSystem3D m_PhysicsSystem3D;
    
    Ref<Texture2D> m_DirLightIcon;
    Ref<Texture2D> m_PointLightIcon;

    friend class SceneSerializer;
    friend class Entity;
    friend class SceneHierarchyPanel;
};

template <> UHE_API void Scene::OnComponentAdded<TransformComponent>(Entity entity, TransformComponent& components);
template <> UHE_API void Scene::OnComponentAdded<CameraComponent>(Entity entity, CameraComponent& components);
template <> UHE_API void Scene::OnComponentAdded<TagComponent>(Entity entity, TagComponent& components);
template <> UHE_API void Scene::OnComponentAdded<SpriteRendererComponent>(Entity entity, SpriteRendererComponent& components);
template <> UHE_API void Scene::OnComponentAdded<TextComponent>(Entity entity, TextComponent& components);
template <> UHE_API void Scene::OnComponentAdded<SpriteAnimationComponent>(Entity entity, SpriteAnimationComponent& components);
template <> UHE_API void Scene::OnComponentAdded<NativeScriptComponent>(Entity entity, NativeScriptComponent& components);
template <> UHE_API void Scene::OnComponentAdded<RigidBody2DComponent>(Entity entity, RigidBody2DComponent& components);
template <> UHE_API void Scene::OnComponentAdded<BoxColliderComponent>(Entity entity, BoxColliderComponent& components);
template <> UHE_API void Scene::OnComponentAdded<IDComponent>(Entity entity, IDComponent& components);
template <> UHE_API void Scene::OnComponentAdded<Model3DComponent>(Entity entity, Model3DComponent& components);
template <> UHE_API void Scene::OnComponentAdded<DirectionalLightComponent>(Entity entity, DirectionalLightComponent& components);
template <> UHE_API void Scene::OnComponentAdded<PointLightComponent>(Entity entity, PointLightComponent& components);
template <> UHE_API void Scene::OnComponentAdded<AnimatorComponent>(Entity entity, AnimatorComponent& components);
template <> UHE_API void Scene::OnComponentAdded<ModelNodeComponent>(Entity entity, ModelNodeComponent& components);
template <> UHE_API void Scene::OnComponentAdded<RelationshipComponent>(Entity entity, RelationshipComponent& components);
template <> UHE_API void Scene::OnComponentAdded<RigidBody3DComponent>(Entity entity, RigidBody3DComponent& components);
template <> UHE_API void Scene::OnComponentAdded<BoxCollider3DComponent>(Entity entity, BoxCollider3DComponent& components);
template <> UHE_API void Scene::OnComponentAdded<SphereCollider3DComponent>(Entity entity, SphereCollider3DComponent& components);
template <> UHE_API void Scene::OnComponentAdded<CapsuleCollider3DComponent>(Entity entity, CapsuleCollider3DComponent& components);

} // namespace UHE
