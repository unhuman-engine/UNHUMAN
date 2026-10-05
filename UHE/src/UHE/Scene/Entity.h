#pragma once
#include "uhepch.h"
#include "UHE/Scene/Components.h"
#include "Scene.h"
#include "UHE/Core/UIID.h"
#include <entt.hpp>

namespace UHE {



	class UHE_API Entity
	{
	public:
		Entity() = default;
		Entity(entt::entity handle, Scene* scene)
			:m_EntityHandle(handle), m_Scene(scene) {};

		Entity(const Entity&) = default;
		~Entity() = default;        UUID GetUUID() { return GetComponent<IDComponent>().ID; }

        // Scene this entity belongs to (null if invalid).
        Scene* GetScene() const { return m_Scene; }

		template<typename T>
		bool HasComponent()
		{
			return m_Scene->m_registry.all_of<T>(m_EntityHandle);
		}

		template<typename T>
		T& GetComponent()
		{
			UHE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			return m_Scene->m_registry.get<T>(m_EntityHandle);
		}

		template<typename T, typename... Args>
		T& AddComponent(Args&&... args)
		{
			UHE_CORE_ASSERT(!HasComponent<T>(), "Entity already has component!");
			T& component = m_Scene->m_registry.emplace<T>(m_EntityHandle, std::forward<Args>(args)...);
			m_Scene->OnComponentAdded<T>(*this, component);
			return component;
		}

		template<typename T>
		void RemoveComponent()
		{
			UHE_CORE_ASSERT(HasComponent<T>(), "Entity does not have component!");
			m_Scene->m_registry.remove<T>(m_EntityHandle);
		}

		operator bool() const { return m_EntityHandle != entt::null; }
		operator u32() const { return (u32)m_EntityHandle; }
		operator entt::entity() const { return m_EntityHandle; }

		bool operator==(const Entity& other) const
		{
			return m_EntityHandle == other.m_EntityHandle && m_Scene == other.m_Scene;
		}

		bool operator!=(const Entity& other) const
		{
			return !(*this == other);
		}

		bool operator==(entt::entity other) const
		{
			return m_EntityHandle == other;
		}

		bool operator!=(entt::entity other) const
		{
			return !(*this == other);
		}
	private:
		entt::entity m_EntityHandle{ entt::null };
		Scene* m_Scene = nullptr;
	};

}