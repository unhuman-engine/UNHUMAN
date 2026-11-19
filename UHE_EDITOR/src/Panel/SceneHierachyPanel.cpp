#include "SceneHierachyPanel.h"
#include "EditorUI.h"
#include "glm/trigonometric.hpp"
#include "imgui/imgui_internal.h"
#include <imgui/imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "UHE/AssetsManager/VfsSystem.h"
#include "EditorTheme.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include "SceneHierarchyStyle.h"

namespace UHE {


// Issue #17: outliner icons, lazily loaded once per panel instance.
void* SceneHierarchyPanel::LoadOutlinerIcon(const char *assetName) {
  auto it = m_IconCache.find(assetName);
  if (it == m_IconCache.end()) {
    std::string path =
        (FileSystem::Get().GetRootPath() / "assets/icon" / assetName).string();
    Ref<Texture2D> tex = Texture2D::Create(path);
    it = m_IconCache.emplace(assetName, tex).first;
  }
  Ref<Texture2D> &tex = it->second;
  return tex ? tex->GetImGuiTextureID() : nullptr;
}

void* SceneHierarchyPanel::GetEntityIcon(Entity entity) {
  if (entity.HasComponent<CameraComponent>())
    return LoadOutlinerIcon("camera.png");
  if (entity.HasComponent<DirectionalLightComponent>())
    return LoadOutlinerIcon("directionLight.png");
  if (entity.HasComponent<PointLightComponent>())
    return LoadOutlinerIcon("pointLight.png");
  if (entity.HasComponent<SpotLightComponent>())
    return LoadOutlinerIcon("directionLight.png");
  if (entity.HasComponent<SpriteRendererComponent>() ||
      entity.HasComponent<SpriteAnimationComponent>())
    return LoadOutlinerIcon("image.png");
  if (entity.HasComponent<TextComponent>())
    return LoadOutlinerIcon("text.png");
  if (entity.HasComponent<ModelNodeComponent>()) {
    bool group = entity.GetComponent<ModelNodeComponent>().HasChildrenNodes;
    return group ? LoadOutlinerIcon("folder.png") : LoadOutlinerIcon("3dmodel.png");
  }
  if (entity.HasComponent<Model3DComponent>())
    return LoadOutlinerIcon("3dmodel.png");
  if (entity.HasComponent<RelationshipComponent>() &&
      !entity.GetComponent<RelationshipComponent>().Children.empty())
    return LoadOutlinerIcon("folder.png");
  return LoadOutlinerIcon("file.png");
}

SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene> &context) {
  SetContext(context);
}

void SceneHierarchyPanel::SetContext(const Ref<Scene> &context) {
  // Issue #17: keep expand-state when only swapping to a copy of the same
  // scene (play mode), reset it when actually switching scenes.
  if (m_Context != context) {
    m_ExpandedEntities.clear();
    m_SelectionContext = {};
  }
  m_Context = context;
}

// panel renderer st from here
// Issue #17: apply the mutations queued by the tree UI. Runs after all
// ImGui calls, outside registry iteration. The Editor calls it at the end of
// OnImGuiRender so nothing draws against a half-updated scene.
void SceneHierarchyPanel::ApplyQueuedMutations() {
  if (!m_PendingCreateParents.empty()) {
    for (u64 parentID : m_PendingCreateParents) {
      if (parentID == 0) {
        m_Context->CreateEntity("Empty Entity");
      } else {
        Entity parent = m_Context->GetEntityWithUUID(parentID);
        if (parent)
          m_Context->CreateChildEntity(parent, "Empty Entity");
      }
    }
    m_PendingCreateParents.clear();
  }

  if (!m_PendingDeleteIDs.empty()) {
    for (u64 deleteID : m_PendingDeleteIDs) {
      Entity toDelete = m_Context->GetEntityWithUUID(deleteID);
      if (toDelete) {
        // Clear the selection BEFORE destroying when it is the deleted entity
        // OR a descendant of it (deleting a parent removes its whole subtree,
        // so a child selection would dangle and crash the properties panel).
        bool clearSelection =
            m_SelectionContext == toDelete ||
            (m_SelectionContext &&
             m_Context->IsEntityParentOf(toDelete, m_SelectionContext));
        if (clearSelection)
          m_SelectionContext = {};

        m_Context->DestroyEntity(toDelete);
      }
    }
    m_PendingDeleteIDs.clear();
  }
}

void SceneHierarchyPanel::OnImGuiRender(bool showHierarchy, bool showInspector) {
  UHE_PROFILE_FUNCTION();
  if (showHierarchy) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::Begin("Scene Hierarchy");

    ImGuiIO &io = ImGui::GetIO();
    ImFont *boldFont = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;

    if (boldFont)
      ImGui::PushFont(boldFont);
    ImGui::TextColored(DimText(0.60f), "ENTITIES");
    if (boldFont)
      ImGui::PopFont();

    ImGui::SameLine();
    ImGui::TextDisabled("%zu", m_Context ? m_Context->GetRegistry().view<entt::entity>().size() : 0);

    const float buttonsWidth = 172.0f;
    ImGui::SameLine(ImGui::GetWindowWidth() - buttonsWidth);
    if (ImGui::SmallButton("Expand All")) {
      for (Entity root : m_Context->GetRootEntities())
        ExpandAll(root);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Expand every entity branch");
    ImGui::SameLine();
    if (ImGui::SmallButton("Collapse"))
      m_ExpandedEntities.clear();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Collapse every entity branch");

    EditorUI::SearchField("##HierarchySearch", m_SearchBuffer, sizeof(m_SearchBuffer),
                          "Search entities...", -1.0f);
    ImGui::Spacing();

    std::string needle = m_SearchBuffer;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool filtering = !needle.empty();

    // Issue #17: draw the Scene's deduplicated root list; children render
    // recursively inside DrawEntityNode. Snapshotting the roots once per frame
    // means mutations queued inside popups can't corrupt what we iterate.
    std::vector<Entity> roots = m_Context->GetRootEntities();
    int rowIdx = 0;
    size_t visibleCount = 0;
    for (Entity entity : roots) {
      if (filtering && !SubtreeMatchesFilter(entity, needle))
        continue;
      if (rowIdx % 2 == 1) {
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImVec2 regionAvail = ImGui::GetContentRegionAvail();
        float rowHeight =
            ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2.0f + 2.0f;
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursorPos,
            ImVec2(cursorPos.x + regionAvail.x, cursorPos.y + rowHeight),
            ImGui::GetColorU32(ImGuiCol_TableRowBgAlt), 0.0f);
      }
      DrawEntityNode(entity);
      rowIdx++;
      visibleCount++;
    }

    if (filtering && visibleCount == 0)
      ImGui::TextDisabled("No entities match \"%s\"", m_SearchBuffer);

    // Issue #17: "unparent to root" drop zone, registered BEFORE the tree rows
    // so a drop on a row prefers that row's own target; this one only wins over
    // empty panel space.
    {
      ImVec2 regionMin = ImGui::GetWindowContentRegionMin();
      ImVec2 regionMax = ImGui::GetWindowContentRegionMax();
      ImVec2 origin = ImGui::GetWindowPos();
      ImRect bb(ImVec2(origin.x + regionMin.x, origin.y + regionMin.y),
                ImVec2(origin.x + regionMax.x, origin.y + regionMax.y));
      if (ImGui::BeginDragDropTargetCustom(bb, ImGui::GetID("HierarchyDropToRoot"))) {
        if (const ImGuiPayload *payload =
                ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
          m_Context->QueueCollapse(*(const u64 *)payload->Data);
        ImGui::EndDragDropTarget();
      }
    }

    if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered() &&
        !ImGui::IsAnyItemHovered())
      m_SelectionContext = {};
    if (ImGui::BeginPopupContextWindow("HierarchyContextMenu",
                                       ImGuiPopupFlags_MouseButtonRight |
                                           ImGuiPopupFlags_NoOpenOverItems)) {
      DrawCreateEntityMenu({});
      ImGui::EndPopup();
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }

  if (showInspector) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
    ImGui::Begin("Properties");
    if (m_SelectionContext) {
      auto &tagComponent = m_SelectionContext.GetComponent<TagComponent>();
      ImGuiIO &io = ImGui::GetIO();
      ImFont *boldFont = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
      if (boldFont)
        ImGui::PushFont(boldFont);
      ImGui::TextColored(::EditorTheme::Accent(), "%s", tagComponent.Tag.c_str());
      if (boldFont)
        ImGui::PopFont();
      ImGui::SameLine();
      ImGui::TextDisabled("#%llu", (unsigned long long)m_SelectionContext.GetUUID());
      ImGui::Separator();
      DrawComponents(m_SelectionContext);
    } else {
      ImGui::TextDisabled("Select an entity to view properties");
      ImGui::Spacing();
      ImGui::TextWrapped("Use the Hierarchy to pick an entity, or create one from the "
                         "Create menu in the main menu bar.");
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }
}

bool SceneHierarchyPanel::MatchesFilter(Entity entity, const std::string& needle) {
  if (needle.empty() || !entity.HasComponent<TagComponent>())
    return false;

  std::string tag = entity.GetComponent<TagComponent>().Tag;
  std::transform(tag.begin(), tag.end(), tag.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return tag.find(needle) != std::string::npos;
}

bool SceneHierarchyPanel::SubtreeMatchesFilter(Entity entity, const std::string& needle) {
  if (MatchesFilter(entity, needle))
    return true;

  if (!entity.HasComponent<RelationshipComponent>())
    return false;

  for (u64 childID : entity.GetComponent<RelationshipComponent>().Children) {
    Entity child = m_Context->GetEntityWithUUID(childID);
    if (child && SubtreeMatchesFilter(child, needle))
      return true;
  }
  return false;
}

void SceneHierarchyPanel::ExpandAll(Entity entity) {
  if (!entity)
    return;

  if (entity.HasComponent<RelationshipComponent>() &&
      !entity.GetComponent<RelationshipComponent>().Children.empty()) {
    m_ExpandedEntities.insert(entity.GetUUID());
    for (u64 childID : entity.GetComponent<RelationshipComponent>().Children) {
      Entity child = m_Context->GetEntityWithUUID(childID);
      if (child)
        ExpandAll(child);
    }
  }
}

void SceneHierarchyPanel::SetSelectedEntity(Entity entity) {
  m_SelectionContext = entity;
}

// Issue #17: shared "create entity" submenu used by the window context menu
// (root level) and each node's context menu (as child of that node).
// Creation is QUEUED, not executed: this runs inside the hierarchy iteration
// and creating an entity immediately would emplace into component pools and
// invalidate the very views being drawn (crashes / duplicated rows).
void SceneHierarchyPanel::DrawCreateEntityMenu(Entity parent) {
  if (ImGui::MenuItem("  Create Empty Entity")) {
    if (parent)
      m_PendingCreateParents.push_back(parent.GetUUID());
    else
      m_PendingCreateParents.push_back(0);
    ImGui::CloseCurrentPopup();
  }
}

void SceneHierarchyPanel::DrawEntityNode(Entity entity) {
  auto &tc = entity.GetComponent<TagComponent>();
  bool hasChildren = entity.HasComponent<RelationshipComponent>() &&
                     !entity.GetComponent<RelationshipComponent>().Children.empty();

  // Issue #17: expand-state persistence, keyed by UUID (stable across
  // entity-vector rebuilds and play-mode scene copies).
  u64 entityID = entity.GetUUID();
  bool expanded = m_ExpandedEntities.count(entityID) != 0;

  std::string needle = m_SearchBuffer;
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const bool filtering = !needle.empty();
  const bool forceOpen = filtering && hasChildren && SubtreeMatchesFilter(entity, needle);

  ImGui::PushID((uint32_t)entity);
  ImGuiTreeNodeFlags flags =
      ((m_SelectionContext == entity) ? ImGuiTreeNodeFlags_Selected : 0) |
      ImGuiTreeNodeFlags_OpenOnArrow;
  flags |= ImGuiTreeNodeFlags_SpanFullWidth;
  // Issue #17: leaf nodes draw without an arrow/expand affordance.
  if (!hasChildren)
    flags |= ImGuiTreeNodeFlags_Leaf;
  if (m_SelectionContext == entity) {
    ImGui::PushStyleColor(ImGuiCol_Header, kAccentMuted());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kAccentMuted());
  }
  if (MatchesFilter(entity, needle))
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent());
  // ImGuiTreeNodeFlags_DefaultOpen only seeds the state the first time a node is
  // drawn, so Expand All and the search reveal could not reopen a node the user
  // had already collapsed. Force the state instead; the click handling below
  // keeps m_ExpandedEntities in sync (SetNextItemOpen does not report a toggle).
  ImGui::SetNextItemOpen(expanded || forceOpen, ImGuiCond_Always);
  bool opened = ImGui::TreeNodeEx(tc.Tag.c_str(), flags);
  if (MatchesFilter(entity, needle))
    ImGui::PopStyleColor();
  if (m_SelectionContext == entity)
    ImGui::PopStyleColor(2);
  if (ImGui::IsItemClicked())
    m_SelectionContext = entity;

  // Track arrow toggles so the expanded set follows the user's clicks.
  if (hasChildren && ImGui::IsItemToggledOpen())
    SetExpanded(entityID, !expanded);

  // Issue #17: outliner type icon, drawn straight into the row's draw list at
  // the right edge of the row. Deliberately NOT an ImGui item: ImGui::Image()
  // submits no ID and would clobber LastItemData, which BeginPopupContextItem
  // and the drag-drop helpers rely on (assert 'id != 0' crash).
  ImTextureID icon = static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(GetEntityIcon(entity)));
  if (icon) {
    ImVec2 rowMin = ImGui::GetItemRectMin();
    ImVec2 rowMax = ImGui::GetItemRectMax();
    ImVec2 iconMin(rowMax.x - 20.0f, (rowMin.y + rowMax.y) * 0.5f - 7.0f);
    ImVec2 iconMax(iconMin.x + 14.0f, iconMin.y + 14.0f);
    ImGui::GetWindowDrawList()->AddImage(icon, iconMin, iconMax, ImVec2(0, 1), ImVec2(1, 0));
  }

  // Issue #17: drag source + drop target for re-parenting in the tree.
  // Mutations are QUEUED and applied by the Editor after OnImGuiRender —
  // executing them here corrupted the tree mid-draw (duplication/crashes).
  if (ImGui::BeginDragDropSource()) {
    u64 dragID = entityID;
    ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &dragID, sizeof(u64));
    ImGui::TextUnformatted(tc.Tag.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
      m_Context->QueueReparent(*(const u64 *)payload->Data, entityID);
    ImGui::EndDragDropTarget();
  }

  bool entityDeleted = false;
  if (ImGui::BeginPopupContextItem()) {
    ImGui::TextColored(DimText(0.50f), "Entity Actions");
    ImGui::Separator();
    DrawCreateEntityMenu(entity);
    if (ImGui::MenuItem("  Unparent to Root"))
      m_Context->QueueCollapse(entityID);
    ImGui::Separator();
    if (ImGui::MenuItem("  Delete Entity"))
      entityDeleted = true;
    ImGui::EndPopup();
  }

  // Issue #17: recurse into children so the tree reflects the hierarchy.
  // Children are re-resolved through their UUID so a stale entry can never
  // draw a dangling handle.
  if (opened) {
    if (hasChildren) {
      auto childrenCopy = entity.GetComponent<RelationshipComponent>().Children;
      for (u64 childID : childrenCopy) {
        Entity child = m_Context->GetEntityWithUUID(childID);
        // Edge validation: a child entry only draws when the child's own
        // Parent points back at us — stale entries (child re-parented or
        // deleted elsewhere) can never produce a duplicate row.
        if (child && child.HasComponent<RelationshipComponent>() &&
            child.GetComponent<RelationshipComponent>().Parent == entityID) {
          if (filtering && !SubtreeMatchesFilter(child, needle))
            continue;
          DrawEntityNode(child);
        }
      }
    }
    ImGui::TreePop();
  }
  ImGui::PopID();

  // Defer deletion until the whole node subtree has finished drawing.
  if (entityDeleted)
    m_PendingDeleteIDs.push_back(entityID);
}

} // namespace UHE
