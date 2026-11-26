#pragma once
#include "uhepch.h"
#include <entt.hpp>
#include <typeinfo>
#include "Scene.h"
#include "UHE/Core/UUID.h"
#include "UHE/Scene/Components.h"

namespace UHE
{

/**
 * @brief A handle to one entity of one scene, safe to keep across frames.
 *
 * It remembers which Scene instance it came from (Scene::GetInstanceId), so
 * it knows when that scene was destroyed (Play and Stop replace the active
 * scene; opening a scene or a project does too) or the entity was. Such a
 * handle is invalid: `if (entity)` and IsValid() are false, HasComponent is
 * false and TryGetComponent returns null, where reaching through the old
 * Scene* crashed the editor. GetComponent still requires a live entity that
 * has the component: check first, or use TryGetComponent.
 */
class UHE_API Entity
{
public:
    Entity() = default;
    Entity(entt::entity handle, Scene* scene)
        : m_EntityHandle(handle), m_Scene(scene), m_SceneId(scene != nullptr ? scene->GetInstanceId() : 0)
    {
    }

    Entity(const Entity&) = default;
    Entity& operator=(const Entity&) = default;
    ~Entity() = default;
    UUID GetUUID() { return GetComponent<IDComponent>().ID; }

    /// @brief The scene still exists and still has this entity.
    [[nodiscard]] bool IsValid() const
    {
        return m_EntityHandle != entt::null && m_Scene != nullptr && Scene::IsAlive(m_SceneId) &&
               m_Scene->m_registry.valid(m_EntityHandle);
    }

    // Scene this entity belongs to; null when it no longer exists.
    Scene* GetScene() const { return Scene::IsAlive(m_SceneId) ? m_Scene : nullptr; }

    /// @brief False for an invalid entity, too.
    template <typename T> bool HasComponent() const
    {
        return IsValid() && m_Scene->m_registry.all_of<T>(m_EntityHandle);
    }

    template <typename T> T& GetComponent()
    {
        UHE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
        return m_Scene->m_registry.get<T>(m_EntityHandle);
    }

    /// @brief The component, or null when the entity is gone or does not have one.
    template <typename T> T* TryGetComponent()
    {
        return IsValid() ? m_Scene->m_registry.try_get<T>(m_EntityHandle) : nullptr;
    }

    /// @brief Adds a T built from \p args; an entity that already has one keeps it, unchanged.
    ///
    /// UHE_CORE_ASSERT is compiled out of non-debug builds, and entt asserts
    /// (aborts) on a second emplace: adding a component twice from the
    /// editor's menu ended the process.
    template <typename T, typename... Args> T& AddComponent(Args&&... args)
    {
        if (HasComponent<T>())
        {
            UHE_CORE_WARN("Entity already has component {0}; keeping the existing one", typeid(T).name());
            return GetComponent<T>();
        }
        T& component = m_Scene->m_registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
        m_Scene->OnComponentAdded<T>(*this, component);
        return component;
    }

    template <typename T> void RemoveComponent()
    {
        UHE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
        m_Scene->m_registry.remove<T>(m_EntityHandle);
    }

    /// @brief IsValid(): a handle whose scene or entity is gone is false.
    operator bool() const { return IsValid(); }
    operator u32() const { return (u32)m_EntityHandle; }
    operator entt::entity() const { return m_EntityHandle; }

    bool operator==(const Entity& other) const
    {
        return m_EntityHandle == other.m_EntityHandle && m_Scene == other.m_Scene;
    }

    bool operator!=(const Entity& other) const { return !(*this == other); }

    bool operator==(entt::entity other) const { return m_EntityHandle == other; }

    bool operator!=(entt::entity other) const { return !(*this == other); }

private:
    entt::entity m_EntityHandle{entt::null};
    Scene* m_Scene = nullptr;
    u64 m_SceneId = 0; ///< Scene::GetInstanceId of m_Scene when this was made
};

} // namespace UHE