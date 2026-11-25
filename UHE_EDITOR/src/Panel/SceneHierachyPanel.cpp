#include "SceneHierachyPanel.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <imgui/imgui.h>
#include <imgui_internal.h>
#include <numbers>
#include "EditorTheme.h"
#include "EditorUI.h"
#include "SceneHierarchyStyle.h"

namespace UHE
{

namespace
{
/// Row metrics of the tree. Compact on purpose (Godot's Scene dock, Blender's Outliner): the theme's FramePadding is
/// sized for buttons and made every row half again as tall as its text.
constexpr ImVec2 kRowPadding{4.0f, 3.0f};
/// Space between a row's type icon and its name.
constexpr float kIconGap = 5.0f;
/// Space between the toolbar's controls.
constexpr float kToolbarGap = 4.0f;

/// @brief What an entity is, as far as its outliner icon is concerned.
enum class EntityKind : std::uint8_t
{
    Camera,
    DirectionalLight,
    PointLight,
    SpotLight,
    Sprite,
    Text,
    Mesh,
    Group,
    Empty,
};

enum class ToolbarIcon : std::uint8_t
{
    Add,
    ExpandAll,
    CollapseAll,
};

/// @brief Lower-cases \p text for the case-insensitive filter.
std::string ToLower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// @brief Classifies \p entity by the component that best describes it, in the order a reader would name it.
EntityKind ClassifyEntity(Entity entity)
{
    if (entity.HasComponent<CameraComponent>())
        return EntityKind::Camera;
    if (entity.HasComponent<DirectionalLightComponent>())
        return EntityKind::DirectionalLight;
    if (entity.HasComponent<PointLightComponent>())
        return EntityKind::PointLight;
    if (entity.HasComponent<SpotLightComponent>())
        return EntityKind::SpotLight;
    if (entity.HasComponent<SpriteRendererComponent>() || entity.HasComponent<SpriteAnimationComponent>())
        return EntityKind::Sprite;
    if (entity.HasComponent<TextComponent>())
        return EntityKind::Text;
    if (entity.HasComponent<ModelNodeComponent>())
        return entity.GetComponent<ModelNodeComponent>().HasChildrenNodes ? EntityKind::Group : EntityKind::Mesh;
    if (entity.HasComponent<Model3DComponent>())
        return EntityKind::Mesh;
    if (entity.HasComponent<RelationshipComponent>() && !entity.GetComponent<RelationshipComponent>().Children.empty())
        return EntityKind::Group;
    return EntityKind::Empty;
}

/// @brief True when the active theme has a light background.
/// @details The type colours need a darker variant there: a warm yellow that reads on grey vanishes on white.
bool IsLightTheme()
{
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    return 0.2126f * bg.x + 0.7152f * bg.y + 0.0722f * bg.z > 0.5f;
}

/// @brief The colour \p t of the way from the panel background to \p to, as an opaque colour.
/// @details The swapchain is sRGB, so blending happens in linear light: a 2.5% white overlay on a dark panel came out
/// about twice as bright as the background (the harsh zebra rows). Mixing here, in sRGB, draws the band exactly the
/// colour that was authored, on dark and light themes alike.
ImVec4 PanelMix(const ImVec4& to, float t)
{
    ImVec4 c = ImLerp(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), to, t);
    c.w = 1.0f;
    return c;
}

/// @brief The colour of \p kind's icon, in sRGB, for the active theme.
/// @details Lights, cameras, sprites and text get a hue each so a scene can be scanned by type; meshes, groups and
/// empty entities stay neutral because they are most of a scene and colour would stop meaning anything.
ImVec4 EntityKindColor(EntityKind kind)
{
    const bool light = IsLightTheme();
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    switch (kind)
    {
        case EntityKind::DirectionalLight:
        case EntityKind::PointLight:
        case EntityKind::SpotLight:
            return light ? ImVec4(0.72f, 0.50f, 0.02f, 1.0f) : ImVec4(1.00f, 0.82f, 0.38f, 1.0f);
        case EntityKind::Camera:
            return light ? ImVec4(0.38f, 0.34f, 0.80f, 1.0f) : ImVec4(0.66f, 0.64f, 1.00f, 1.0f);
        case EntityKind::Sprite:
            return light ? ImVec4(0.15f, 0.52f, 0.29f, 1.0f) : ImVec4(0.50f, 0.84f, 0.60f, 1.0f);
        case EntityKind::Text:
            return light ? ImVec4(0.08f, 0.46f, 0.60f, 1.0f) : ImVec4(0.48f, 0.80f, 0.92f, 1.0f);
        case EntityKind::Mesh:
            return PanelMix(text, 0.80f);
        case EntityKind::Group:
            return PanelMix(text, 0.62f);
        case EntityKind::Empty:
            return PanelMix(text, 0.45f);
    }
    return text;
}

/// @brief Draws \p kind's icon centred on \p c in a \p size pixel square.
/// @details Drawn with the draw list rather than loaded from bitmaps (the editor has no icon font) so it stays sharp at
/// any font size and takes its colour from the theme; the old PNGs had fixed colours that some themes swallowed.
void DrawEntityIcon(ImDrawList* draw, const ImVec2& c, float size, EntityKind kind, ImU32 col)
{
    const float u = size;
    const float t = ImMax(1.0f, size / 11.0f);
    // Coordinates below are in units of the icon size, centred, with y pointing down.
    auto p = [&](float x, float y) { return ImVec2(c.x + x * u, c.y + y * u); };

    switch (kind)
    {
        case EntityKind::Camera:
            draw->AddCircleFilled(p(-0.26f, -0.23f), 0.13f * u, col, 12);
            draw->AddCircleFilled(p(0.00f, -0.23f), 0.13f * u, col, 12);
            draw->AddRectFilled(p(-0.42f, -0.08f), p(0.14f, 0.36f), col, 0.08f * u);
            draw->AddTriangleFilled(p(0.10f, 0.14f), p(0.44f, -0.04f), p(0.44f, 0.34f), col);
            break;
        case EntityKind::DirectionalLight:
            draw->AddCircleFilled(c, 0.19f * u, col, 16);
            for (int i = 0; i < 8; ++i)
            {
                const float angle = static_cast<float>(i) * (std::numbers::pi_v<float> / 4.0f);
                const float dx = std::cos(angle);
                const float dy = std::sin(angle);
                draw->AddLine(p(dx * 0.30f, dy * 0.30f), p(dx * 0.45f, dy * 0.45f), col, t);
            }
            break;
        case EntityKind::PointLight:
            draw->AddCircleFilled(p(0.0f, -0.10f), 0.28f * u, col, 16);
            draw->AddRectFilled(p(-0.13f, 0.12f), p(0.13f, 0.30f), col);
            draw->AddRectFilled(p(-0.09f, 0.34f), p(0.09f, 0.42f), col, 0.03f * u);
            break;
        case EntityKind::SpotLight:
            draw->AddRectFilled(p(-0.14f, -0.42f), p(0.14f, -0.22f), col, 0.04f * u);
            draw->AddLine(p(-0.12f, -0.20f), p(-0.40f, 0.36f), col, t);
            draw->AddLine(p(0.12f, -0.20f), p(0.40f, 0.36f), col, t);
            draw->AddLine(p(-0.40f, 0.36f), p(0.40f, 0.36f), col, t);
            break;
        case EntityKind::Sprite:
            draw->AddRect(p(-0.42f, -0.34f), p(0.42f, 0.34f), col, 0.08f * u, 0, t);
            draw->AddTriangleFilled(p(-0.30f, 0.24f), p(-0.08f, -0.06f), p(0.14f, 0.24f), col);
            draw->AddTriangleFilled(p(0.02f, 0.24f), p(0.17f, 0.04f), p(0.32f, 0.24f), col);
            draw->AddCircleFilled(p(0.18f, -0.15f), 0.07f * u, col, 10);
            break;
        case EntityKind::Text:
            draw->AddRectFilled(p(-0.34f, -0.38f), p(0.34f, -0.24f), col);
            draw->AddRectFilled(p(-0.07f, -0.24f), p(0.07f, 0.38f), col);
            break;
        case EntityKind::Mesh:
        {
            // An isometric cube: solid top face, outlined sides.
            const std::array<ImVec2, 6> hull = {p(0.0f, -0.44f), p(0.40f, -0.22f), p(0.40f, 0.22f),
                                                p(0.0f, 0.44f),  p(-0.40f, 0.22f), p(-0.40f, -0.22f)};
            const ImVec2 front = p(0.0f, 0.0f);
            draw->AddQuadFilled(hull[0], hull[1], front, hull[5], col);
            draw->AddPolyline(hull.data(), static_cast<int>(hull.size()), col, ImDrawFlags_Closed, t);
            draw->AddLine(front, hull[3], col, t);
            break;
        }
        case EntityKind::Group:
            draw->AddRectFilled(p(-0.42f, -0.34f), p(-0.04f, -0.16f), col, 0.06f * u, ImDrawFlags_RoundCornersTop);
            draw->AddRect(p(-0.42f, -0.20f), p(0.42f, 0.34f), col, 0.06f * u, 0, t);
            break;
        case EntityKind::Empty:
            draw->AddCircle(c, 0.30f * u, col, 16, t);
            break;
    }
}

/// @brief Draws a toolbar glyph centred on \p c, \p size pixels across.
void DrawToolbarIcon(ImDrawList* draw, const ImVec2& c, float size, ToolbarIcon icon, ImU32 col)
{
    const float t = ImMax(1.0f, size / 10.0f);
    // A "v" (down) or "^" (up) whose middle sits \p y icon-sizes below the centre.
    auto chevron = [&](float y, bool down)
    {
        const float dy = down ? 0.12f : -0.12f;
        const std::array<ImVec2, 3> pts = {ImVec2(c.x - 0.26f * size, c.y + (y - dy) * size),
                                           ImVec2(c.x, c.y + (y + dy) * size),
                                           ImVec2(c.x + 0.26f * size, c.y + (y - dy) * size)};
        draw->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, t);
    };

    switch (icon)
    {
        case ToolbarIcon::Add:
            draw->AddLine(ImVec2(c.x - 0.30f * size, c.y), ImVec2(c.x + 0.30f * size, c.y), col, t * 1.2f);
            draw->AddLine(ImVec2(c.x, c.y - 0.30f * size), ImVec2(c.x, c.y + 0.30f * size), col, t * 1.2f);
            break;
        case ToolbarIcon::ExpandAll:
            chevron(-0.13f, true);
            chevron(0.13f, true);
            break;
        case ToolbarIcon::CollapseAll:
            chevron(-0.13f, false);
            chevron(0.13f, false);
            break;
    }
}

/// @brief A flat square button, as tall as a frame, so it lines up with the filter field beside it.
/// @return True when clicked.
bool ToolbarButton(const char* id, ToolbarIcon icon, const char* tooltip)
{
    const float size = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || held)
        draw->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size),
                            ImGui::GetColorU32(held ? ImGuiCol_FrameBgActive : ImGuiCol_FrameBgHovered),
                            ImGui::GetStyle().FrameRounding);

    const ImU32 foreground = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    DrawToolbarIcon(draw, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), ImGui::GetFontSize(), icon, foreground);

    ImGui::SetItemTooltip("%s", tooltip);
    return clicked;
}
} // namespace

SceneHierarchyPanel::SceneHierarchyPanel(const Ref<Scene>& context)
{
    SetContext(context);
}

void SceneHierarchyPanel::SetContext(const Ref<Scene>& context)
{
    // Issue #17: keep expand-state when only swapping to a copy of the same
    // scene (play mode), reset it when actually switching scenes.
    if (m_Context != context)
    {
        m_ExpandedEntities.clear();
        m_SelectionContext = {};
    }
    m_Context = context;
}

// Issue #17: apply the mutations queued by the tree UI. Runs after all
// ImGui calls, outside registry iteration. The Editor calls it at the end of
// OnImGuiRender so nothing draws against a half-updated scene.
void SceneHierarchyPanel::ApplyQueuedMutations()
{
    if (!m_PendingCreateParents.empty())
    {
        for (u64 parentID : m_PendingCreateParents)
        {
            if (parentID == 0)
            {
                m_Context->CreateEntity("Empty Entity");
            }
            else
            {
                Entity parent = m_Context->GetEntityWithUUID(parentID);
                if (parent)
                    m_Context->CreateChildEntity(parent, "Empty Entity");
            }
        }
        m_PendingCreateParents.clear();
    }

    if (!m_PendingDeleteIDs.empty())
    {
        for (u64 deleteID : m_PendingDeleteIDs)
        {
            Entity toDelete = m_Context->GetEntityWithUUID(deleteID);
            if (toDelete)
            {
                // Clear the selection BEFORE destroying when it is the deleted entity
                // OR a descendant of it (deleting a parent removes its whole subtree,
                // so a child selection would dangle and crash the properties panel).
                bool clearSelection = m_SelectionContext == toDelete ||
                                      (m_SelectionContext && m_Context->IsEntityParentOf(toDelete, m_SelectionContext));
                if (clearSelection)
                    m_SelectionContext = {};

                m_Context->DestroyEntity(toDelete);
            }
        }
        m_PendingDeleteIDs.clear();
    }
}

void SceneHierarchyPanel::OnImGuiRender(bool showHierarchy, bool showInspector)
{
    UHE_PROFILE_FUNCTION();
    if (showHierarchy)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
        ImGui::Begin("Scene Hierarchy");
        if (m_Context)
        {
            DrawToolbar();
            DrawTree();
        }
        else
        {
            ImGui::TextDisabled("No scene loaded");
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    if (showInspector)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
        ImGui::Begin("Properties");
        if (m_SelectionContext)
        {
            auto& tagComponent = m_SelectionContext.GetComponent<TagComponent>();
            ImGuiIO& io = ImGui::GetIO();
            ImFont* boldFont = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
            if (boldFont)
                ImGui::PushFont(boldFont);
            ImGui::TextColored(::EditorTheme::Accent(), "%s", tagComponent.Tag.c_str());
            if (boldFont)
                ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextDisabled("#%llu", static_cast<unsigned long long>(m_SelectionContext.GetUUID()));
            ImGui::Separator();
            DrawComponents(m_SelectionContext);
        }
        else
        {
            ImGui::TextDisabled("Select an entity to view properties");
            ImGui::Spacing();
            ImGui::TextWrapped("Use the Hierarchy to pick an entity, or create one from the "
                               "Create menu in the main menu bar.");
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

void SceneHierarchyPanel::DrawToolbar()
{
    const float buttonSize = ImGui::GetFrameHeight();

    if (ToolbarButton("##HierarchyCreate", ToolbarIcon::Add, "Create an empty entity"))
        m_PendingCreateParents.push_back(0);

    // The entity count lives in the filter's hint: it is what the filter searches, and a separate title row cost a
    // whole line of a narrow dock for one number.
    ImGui::SameLine(0.0f, kToolbarGap);
    const size_t entityCount = m_Context->GetRegistry().view<entt::entity>().size();
    std::array<char, 64> hint{};
    std::snprintf(hint.data(), hint.size(), "Filter %zu %s", entityCount, entityCount == 1 ? "entity" : "entities");
    const float searchWidth = ImMax(ImGui::GetContentRegionAvail().x - 2.0f * (buttonSize + kToolbarGap), 1.0f);
    EditorUI::SearchField("##HierarchySearch", m_SearchBuffer, sizeof(m_SearchBuffer), hint.data(), searchWidth);

    ImGui::SameLine(0.0f, kToolbarGap);
    if (ToolbarButton("##HierarchyExpandAll", ToolbarIcon::ExpandAll, "Expand all"))
    {
        for (Entity root : m_Context->GetRootEntities())
            ExpandAll(root);
    }
    ImGui::SameLine(0.0f, kToolbarGap);
    if (ToolbarButton("##HierarchyCollapseAll", ToolbarIcon::CollapseAll, "Collapse all"))
        m_ExpandedEntities.clear();
}

void SceneHierarchyPanel::DrawTree()
{
    const std::string needle = ToLower(m_SearchBuffer);
    const bool filtering = !needle.empty();

    // Borderless, so ImGui gives it no padding: selection bands run edge to edge, the way an outliner's do.
    ImGui::BeginChild("##HierarchyTree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);

    // Issue #17: draw the Scene's deduplicated root list; children render
    // recursively inside DrawEntityNode. Snapshotting the roots once per frame
    // means mutations queued inside popups can't corrupt what we iterate.
    std::vector<Entity> roots = m_Context->GetRootEntities();
    size_t visibleCount = 0;
    for (Entity entity : roots)
    {
        if (filtering && !SubtreeMatchesFilter(entity, needle))
            continue;
        DrawEntityNode(entity, needle);
        visibleCount++;
    }

    if (visibleCount == 0)
    {
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        ImGui::Indent(kRowPadding.x * 2.0f);
        if (filtering)
            ImGui::TextDisabled("No entities match \"%s\"", m_SearchBuffer);
        else
            ImGui::TextDisabled("Empty scene. Right-click or + to add an entity.");
        ImGui::Unindent(kRowPadding.x * 2.0f);
    }

    // Issue #17: "unparent to root" drop zone over the whole visible tree. A
    // row's own target still wins a drop on that row: ImGui hands the payload
    // to the smallest target rect under the mouse.
    {
        // Through GetCurrentContext(): GImGui, which the inline helpers read, is
        // not exported from the engine DLL on Windows.
        const ImRect bb = ImGui::GetCurrentContext()->CurrentWindow->InnerRect;
        if (ImGui::BeginDragDropTargetCustom(bb, ImGui::GetID("HierarchyDropToRoot")))
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
                m_Context->QueueCollapse(*static_cast<const u64*>(payload->Data));
            ImGui::EndDragDropTarget();
        }
    }

    if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered())
        m_SelectionContext = {};
    if (ImGui::BeginPopupContextWindow("HierarchyContextMenu",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        DrawCreateEntityMenu({});
        ImGui::EndPopup();
    }
    ImGui::EndChild();
}

bool SceneHierarchyPanel::MatchesFilter(Entity entity, const std::string& needle)
{
    if (needle.empty() || !entity.HasComponent<TagComponent>())
        return false;

    return ToLower(entity.GetComponent<TagComponent>().Tag).find(needle) != std::string::npos;
}

bool SceneHierarchyPanel::SubtreeMatchesFilter(Entity entity, const std::string& needle)
{
    if (MatchesFilter(entity, needle))
        return true;

    if (!entity.HasComponent<RelationshipComponent>())
        return false;

    for (u64 childID : entity.GetComponent<RelationshipComponent>().Children)
    {
        Entity child = m_Context->GetEntityWithUUID(childID);
        if (child && SubtreeMatchesFilter(child, needle))
            return true;
    }
    return false;
}

void SceneHierarchyPanel::ExpandAll(Entity entity)
{
    if (!entity)
        return;

    if (entity.HasComponent<RelationshipComponent>() && !entity.GetComponent<RelationshipComponent>().Children.empty())
    {
        m_ExpandedEntities.insert(entity.GetUUID());
        for (u64 childID : entity.GetComponent<RelationshipComponent>().Children)
        {
            Entity child = m_Context->GetEntityWithUUID(childID);
            if (child)
                ExpandAll(child);
        }
    }
}

void SceneHierarchyPanel::SetSelectedEntity(Entity entity)
{
    m_SelectionContext = entity;
}

// Issue #17: shared "create entity" submenu used by the window context menu
// (root level) and each node's context menu (as child of that node).
// Creation is QUEUED, not executed: this runs inside the hierarchy iteration
// and creating an entity immediately would emplace into component pools and
// invalidate the very views being drawn (crashes / duplicated rows).
void SceneHierarchyPanel::DrawCreateEntityMenu(Entity parent)
{
    if (ImGui::MenuItem("  Create Empty Entity"))
    {
        if (parent)
            m_PendingCreateParents.push_back(parent.GetUUID());
        else
            m_PendingCreateParents.push_back(0);
        ImGui::CloseCurrentPopup();
    }
}

void SceneHierarchyPanel::DrawEntityNode(Entity entity, const std::string& needle)
{
    auto& tc = entity.GetComponent<TagComponent>();
    bool hasChildren =
        entity.HasComponent<RelationshipComponent>() && !entity.GetComponent<RelationshipComponent>().Children.empty();

    // Issue #17: expand-state persistence, keyed by UUID (stable across
    // entity-vector rebuilds and play-mode scene copies).
    u64 entityID = entity.GetUUID();
    bool expanded = m_ExpandedEntities.count(entityID) != 0;

    const bool filtering = !needle.empty();
    const bool matches = MatchesFilter(entity, needle);
    const bool forceOpen = filtering && hasChildren && SubtreeMatchesFilter(entity, needle);
    const bool selected = m_SelectionContext == entity;

    ImGui::PushID(static_cast<uint32_t>(entity));
    // Accumulated unsigned, one flag at a time: ImGui's flag typedefs are int,
    // and OR-ing into a signed int is what bugprone-signed-bitwise rejects.
    unsigned flags = ImGuiTreeNodeFlags_OpenOnArrow;
    flags |= ImGuiTreeNodeFlags_OpenOnDoubleClick;
    flags |= ImGuiTreeNodeFlags_SpanFullWidth;
    flags |= ImGuiTreeNodeFlags_FramePadding;
    flags |= ImGuiTreeNodeFlags_NavLeftJumpsToParent;
    if (selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    // Issue #17: leaf nodes draw without an arrow/expand affordance.
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf;

    // The node is submitted with an empty label and the icon and name are drawn
    // after it: ImGui puts a label right after the arrow, leaving no room for an
    // icon in front of the name. With no label, ImGuiCol_Text colours only the
    // arrow, which is dimmed so the names lead.
    const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 accent = ::EditorTheme::Accent();
    const float selectedMix = IsLightTheme() ? 0.22f : 0.30f;
    ImGui::PushStyleColor(ImGuiCol_Text, PanelMix(text, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Header, PanelMix(accent, selectedMix));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          selected ? PanelMix(accent, selectedMix + 0.08f) : PanelMix(text, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                          selected ? PanelMix(accent, selectedMix + 0.08f) : PanelMix(accent, 0.16f));
    // No vertical spacing between rows, so neighbouring bands meet; pushed around
    // the node alone because ItemSpacing also lays out the context menu below.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kRowPadding);

    const float rowX = ImGui::GetCursorScreenPos().x;
    // ImGuiTreeNodeFlags_DefaultOpen only seeds the state the first time a node is
    // drawn, so Expand All and the search reveal could not reopen a node the user
    // had already collapsed. Force the state instead; the click handling below
    // keeps m_ExpandedEntities in sync (SetNextItemOpen does not report a toggle).
    ImGui::SetNextItemOpen(expanded || forceOpen, ImGuiCond_Always);
    bool opened = ImGui::TreeNodeEx("##node", static_cast<ImGuiTreeNodeFlags>(flags));

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    // Track arrow toggles so the expanded set follows the user's clicks. A click
    // that only opened or closed the branch does not select, as in Godot.
    const bool toggled = hasChildren && ImGui::IsItemToggledOpen();
    if (toggled)
        SetExpanded(entityID, !expanded);
    if (ImGui::IsItemClicked() && !toggled)
        m_SelectionContext = entity;

    // Issue #17: type icon and name, drawn straight into the row's draw list.
    // Deliberately NOT ImGui items: ImGui::Image()/Text() would clobber
    // LastItemData, which BeginPopupContextItem and the drag-drop helpers below
    // rely on (assert 'id != 0' crash).
    if (ImGui::IsItemVisible())
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float fontSize = ImGui::GetFontSize();
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        // Where ImGui would have put the label: past the arrow's slot, which leaves keep empty so icons line up.
        const float iconX = rowX + fontSize + kRowPadding.x * 2.0f;
        const float textY = rowMin.y + kRowPadding.y;

        const EntityKind kind = ClassifyEntity(entity);
        DrawEntityIcon(draw, ImVec2(iconX + fontSize * 0.5f, textY + fontSize * 0.5f), fontSize, kind,
                       ImGui::GetColorU32(EntityKindColor(kind)));

        // While filtering, rows shown only as the path to a match are dimmed so the matches stand out.
        ImVec4 nameColor = text;
        if (matches)
            nameColor = kAccent();
        else if (filtering)
            nameColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const ImVec2 textMin(iconX + fontSize + kIconGap, textY);
        const float textMaxX = rowMax.x - kRowPadding.x;
        if (textMaxX > textMin.x)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, nameColor);
            ImGui::RenderTextEllipsis(draw, textMin, ImVec2(textMaxX, textY + fontSize), textMaxX, tc.Tag.c_str(),
                                      nullptr, nullptr);
            ImGui::PopStyleColor();
        }
    }

    // Issue #17: drag source + drop target for re-parenting in the tree.
    // Mutations are QUEUED and applied by the Editor after OnImGuiRender —
    // executing them here corrupted the tree mid-draw (duplication/crashes).
    if (ImGui::BeginDragDropSource())
    {
        u64 dragID = entityID;
        ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &dragID, sizeof(u64));
        ImGui::TextUnformatted(tc.Tag.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY"))
            m_Context->QueueReparent(*static_cast<const u64*>(payload->Data), entityID);
        ImGui::EndDragDropTarget();
    }

    bool entityDeleted = false;
    if (ImGui::BeginPopupContextItem())
    {
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
    if (opened)
    {
        if (hasChildren)
        {
            auto childrenCopy = entity.GetComponent<RelationshipComponent>().Children;
            for (u64 childID : childrenCopy)
            {
                Entity child = m_Context->GetEntityWithUUID(childID);
                // Edge validation: a child entry only draws when the child's own
                // Parent points back at us — stale entries (child re-parented or
                // deleted elsewhere) can never produce a duplicate row.
                if (child && child.HasComponent<RelationshipComponent>() &&
                    child.GetComponent<RelationshipComponent>().Parent == entityID)
                {
                    if (filtering && !SubtreeMatchesFilter(child, needle))
                        continue;
                    DrawEntityNode(child, needle);
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
