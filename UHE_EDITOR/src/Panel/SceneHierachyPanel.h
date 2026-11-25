#pragma once

#include <string>
#include <unordered_set>
#include <vector>
#include "UHE.h"
#include "UHE/Core/Core.h"
#include "UHE/Scene/Entity.h"
#include "UHE/Scene/Scene.h"

namespace UHE
{
class SceneHierarchyPanel
{
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
    /// @brief The one-row header above the tree: create, filter, expand all and collapse all.
    void DrawToolbar();

    /// @brief The scrolling tree, with its empty-space context menu and "unparent to root" drop zone.
    /// @details A child window of its own so the toolbar stays put while a long scene scrolls.
    void DrawTree();

    /// @brief Draws one row and, when it is open, its children.
    /// @param needle The lower-cased filter text, empty when not filtering; passed down so it is built once a frame.
    void DrawEntityNode(Entity entity, const std::string& needle);
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

    // Expand-state keyed by entity UUID (survives registry rebuilds and
    // play-mode scene copies).
    std::unordered_set<u64> m_ExpandedEntities;

    // Mutations queued by the tree UI and applied at the end of
    // OnImGuiRender, outside any registry iteration.
    std::vector<u64> m_PendingCreateParents; // 0 = create at root
    std::vector<u64> m_PendingDeleteIDs;

    char m_SearchBuffer[128] = {};
};
} // namespace UHE
