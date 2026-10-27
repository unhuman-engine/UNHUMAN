#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "UHE.h"
#include "UHE/Core/Core.h"
#include "UHE/Scene/Scene.h"
#include "UHE/Scene/Entity.h"

namespace UHE {
	class SceneHierarchyPanel {
	public:
		SceneHierarchyPanel(const Ref<Scene>& context);

		void SetContext(const Ref<Scene>& context);

		void OnImGuiRender(bool showHierarchy = true, bool showInspector = true);

		Entity GetSelectedEntity() const { return m_SelectionContext; }
		void SetSelectedEntity(Entity entity);

		// Issue #17: apply create/delete mutations queued by the tree UI.
		// Must be called after OnImGuiRender (outside registry iteration).
		void ApplyQueuedMutations();

	private:
		// Issue #17: recursive tree drawing. Root nodes are drawn from
		// OnImGuiRender; children are drawn recursively from their parent node.
		void DrawEntityNode(Entity entity);
		void DrawCreateEntityMenu(Entity parent);
		bool MatchesFilter(Entity entity, const std::string& needle);
		bool SubtreeMatchesFilter(Entity entity, const std::string& needle);
		void ExpandAll(Entity entity);

		void DrawComponents(Entity entity);

		// Issue #17: expand-state persistence for the outliner tree.
		void SetExpanded(u64 entityID, bool expanded)
		{
			if (expanded)
				m_ExpandedEntities.insert(entityID);
			else
				m_ExpandedEntities.erase(entityID);
		}

		Ref<Scene> m_Context;
		Entity m_SelectionContext;

		std::unordered_map<std::string, Ref<Texture2D>> m_IconCache;
		void* LoadOutlinerIcon(const char *assetName);
		void* GetEntityIcon(Entity entity);

		// Expand-state keyed by entity UUID (survives registry rebuilds and
		// play-mode scene copies).
		std::unordered_set<u64> m_ExpandedEntities;

		// Mutations queued by the tree UI and applied at the end of
		// OnImGuiRender, outside any registry iteration.
		std::vector<u64> m_PendingCreateParents; // 0 = create at root
		std::vector<u64> m_PendingDeleteIDs;

		char m_SearchBuffer[128] = {};
	};
}
