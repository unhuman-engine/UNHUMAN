// Component inspector of the scene hierarchy panel: the per-component UI
// drawn for the selected entity. The outliner (tree, filtering, drag and
// drop) is in SceneHierachyPanel.cpp.
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include "EditorTheme.h"
#include "EditorUI.h"
#include "SceneHierachyPanel.h"
#include "SceneHierarchyStyle.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "glm/trigonometric.hpp"
#include "imgui/imgui_internal.h"

namespace UHE
{

/// @brief Narrowest axis field a vector row keeps on one line before it stacks
/// X, Y and Z vertically, as Godot does in a narrow inspector.
static constexpr float kMinAxisFieldWidth = 54.0f;

/// @brief Width of a "Browse" button next to a path field.
static constexpr float kBrowseButtonWidth = 60.0f;

/// @brief Height of the drop zone shown while an asset slot is empty.
static constexpr float kDropZoneHeight = 64.0f;

/// @brief The bold UI font (Inter Bold), or null when only the default font is loaded.
static ImFont* BoldFont()
{
    ImGuiIO& io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
}

/**
 * @brief Opens the two-column label/value table a section draws its fields into.
 *
 * Every section opens its own table with the same proportional label column,
 * so labels and values line up down the whole panel. The cell padding is
 * tighter than the theme's so rows sit about as close as Godot's. Close it
 * with EndPropertyTable() only when this returned true.
 */
static bool BeginPropertyTable(const char* id)
{
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 2.0f));
    // Accumulated unsigned: ImGui's flag typedefs are int, and OR-ing into a
    // signed int is what bugprone-signed-bitwise rejects.
    unsigned flags = ImGuiTableFlags_SizingStretchProp;
    flags |= ImGuiTableFlags_NoSavedSettings;
    flags |= ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable(id, 2, static_cast<ImGuiTableFlags>(flags)))
    {
        ImGui::PopStyleVar();
        return false;
    }
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, kPropertyLabelRatio);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f - kPropertyLabelRatio);
    return true;
}

/// @brief Closes a table opened by BeginPropertyTable(). The cell padding stays
/// pushed until here because the table reads it for every row, not only at begin.
static void EndPropertyTable()
{
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

/**
 * @brief Starts a property row: the name in the left column, the cursor in the right one.
 *
 * The name is drawn by hand instead of with Text() so a name longer than its
 * column ends in an ellipsis rather than running under the value; the full
 * name is then the tooltip. The next item is set to fill the value column, so
 * the caller only gives its widget a hidden "##" label.
 *
 * @param label   Name shown in the label column.
 * @param tooltip Optional help shown when the name is hovered.
 */
static void PropertyLabel(const char* label, const char* tooltip = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImMax(ImGui::GetContentRegionAvail().x, 1.0f);
    const float height = ImGui::GetFrameHeight();
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 textMin(pos.x, pos.y + style.FramePadding.y);
    const ImVec2 textMax(pos.x + width, pos.y + height);

    // RenderTextEllipsis draws with ImGuiCol_Text, so the dimmed colour is pushed.
    ImGui::PushStyleColor(ImGuiCol_Text, PropertyLabelText());
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), textMin, textMax, textMax.x, label, nullptr, &textSize);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(width, height));

    const bool truncated = textSize.x > width;
    if ((truncated || tooltip) && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
    {
        if (truncated && tooltip)
            ImGui::SetTooltip("%s\n%s", label, tooltip);
        else
            ImGui::SetTooltip("%s", truncated ? label : tooltip);
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

/// @brief A read-only property, its value as text aligned with the editable rows' frames.
static void PropertyText(const char* label, const char* fmt, ...) IM_FMTARGS(2);
static void PropertyText(const char* label, const char* fmt, ...)
{
    PropertyLabel(label);
    ImGui::AlignTextToFramePadding();
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

/// @brief A dimmed help line under the values of a section, wrapped to the value column.
static void PropertyNote(const char* text)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

/**
 * @brief One component of a vector property: an axis tag and a drag field in one frame.
 *
 * The tag is a thin coloured stripe and letter rather than a filled button, so
 * a row of three reads as one calm field, as in Godot and Blender. The frame is
 * drawn here across the whole width and the DragFloat's own background is made
 * transparent, so tag and value never show a seam between two rounded frames.
 * Clicking the tag resets the component, as the old axis buttons did.
 */
static void DrawAxisField(int axis, float& component, float resetValue, float width)
{
    static constexpr const char* kAxisLetters[3] = {"X", "Y", "Z"};
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float tagWidth = ImMin(ImGui::GetFontSize() + 4.0f, width * 0.4f);
    const float stripeWidth = 3.0f;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);

    ImGui::PushID(axis);

    // The frame is drawn before the widgets exist, so its hover and active
    // state come from the mouse and the drag's ID rather than from the items.
    const ImGuiID dragId = ImGui::GetID("##value");
    // GetCurrentContext(), not GetActiveID(): that inline helper reads GImGui,
    // which the engine DLL does not export on Windows.
    const bool active = ImGui::GetCurrentContext()->ActiveId == dragId;
    const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max);
    const ImGuiCol frameCol = active ? ImGuiCol_FrameBgActive : (hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, ImGui::GetColorU32(frameCol), style.FrameRounding);
    const ImVec4 axisColor = AxisColor(axis);
    draw->AddRectFilled(min, ImVec2(min.x + stripeWidth, max.y), ImGui::GetColorU32(axisColor), style.FrameRounding,
                        ImDrawFlags_RoundCornersLeft);

    if (ImGui::InvisibleButton("##reset", ImVec2(tagWidth, height)))
        component = resetValue;
    const bool tagHovered = ImGui::IsItemHovered();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Reset %s to %.2f", kAxisLetters[axis], resetValue);

    ImFont* font = BoldFont() ? BoldFont() : ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const char* letter = kAxisLetters[axis];
    const ImVec2 letterSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, letter);
    ImVec4 letterColor = axisColor;
    letterColor.w = tagHovered ? 1.0f : 0.85f;
    const ImVec2 letterPos(min.x + stripeWidth + (tagWidth - stripeWidth - letterSize.x) * 0.5f,
                           min.y + (height - letterSize.y) * 0.5f);
    draw->AddText(font, fontSize, letterPos, ImGui::GetColorU32(letterColor), letter);

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::SetNextItemWidth(ImMax(width - tagWidth, 1.0f));
    ImGui::DragFloat("##value", &component, 0.01f, 0.0f, 0.0f, "%.2f");
    ImGui::PopStyleColor(3);

    ImGui::PopID();
}

/**
 * @brief A vec3 property row: the name in the label column, X/Y/Z fields in the value column.
 *
 * The fields share a line while each keeps a usable width and stack
 * vertically below that, so a narrow panel clips no digits.
 * Must be called inside a BeginPropertyTable() table.
 */
static void DrawVec3Control(const char* label, glm::vec3& value, f32 resetValue = 0.0f)
{
    PropertyLabel(label);
    ImGui::PushID(label);
    const float spacing = 4.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float inlineWidth = (avail - spacing * 2.0f) / 3.0f;
    const bool stacked = inlineWidth < kMinAxisFieldWidth;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 2.0f));
    for (int axis = 0; axis < 3; ++axis)
    {
        if (axis > 0 && !stacked)
            ImGui::SameLine(0.0f, spacing);
        DrawAxisField(axis, value[axis], resetValue, stacked ? avail : inlineWidth);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
}

/**
 * @brief Tree node flags of an inspector section header.
 *
 * The header does not push the tree, so a section's fields span the full
 * panel width and the caller never calls TreePop(). Accumulated unsigned for
 * the same reason as the table flags above.
 */
static ImGuiTreeNodeFlags SectionHeaderFlags()
{
    unsigned flags = ImGuiTreeNodeFlags_DefaultOpen;
    flags |= ImGuiTreeNodeFlags_Framed;
    flags |= ImGuiTreeNodeFlags_SpanAvailWidth;
    flags |= ImGuiTreeNodeFlags_AllowOverlap;
    flags |= ImGuiTreeNodeFlags_FramePadding;
    flags |= ImGuiTreeNodeFlags_NoTreePushOnOpen;
    return static_cast<ImGuiTreeNodeFlags>(flags);
}

/**
 * @brief A collapsible section header in the flat style of Godot's inspector categories.
 *
 * The band uses the theme's neutral button colour rather than the accent-tinted
 * header colour, which is meant for selection; the title is bold at the body
 * size, so every header has the height of a field row.
 *
 * @param id    Tree node ID; it carries the open/closed state between frames.
 * @param title Visible section name.
 * @return Whether the section is open.
 */
static bool SectionHeader(ImGuiID id, const char* title)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, style.FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImFont* boldFont = BoldFont();
    if (boldFont)
        ImGui::PushFont(boldFont, 0.0f);
    const bool open = ImGui::TreeNodeBehavior(id, SectionHeaderFlags(), title);
    if (boldFont)
        ImGui::PopFont();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return open;
}

/**
 * @brief The settings button at the right end of the section header just drawn.
 *
 * Three dots drawn with the draw list, because "..." as text sits on the
 * baseline and looks off-centre in a header band.
 *
 * @return True when clicked.
 */
static bool SectionMenuButton(const char* id)
{
    const ImVec2 headerMin = ImGui::GetItemRectMin();
    const ImVec2 headerMax = ImGui::GetItemRectMax();
    const float size = headerMax.y - headerMin.y;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(headerMax.x - size, headerMin.y));
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Component settings");

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered || held)
        draw->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 2.0f), ImVec2(max.x - 2.0f, max.y - 2.0f),
                            ImGui::GetColorU32(held ? kAccentHover() : kAccentMuted()),
                            ImGui::GetStyle().FrameRounding);
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const float radius = ImMax(1.5f, size * 0.065f);
    const ImU32 dotColor = ImGui::GetColorU32(hovered ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : DimText(0.65f));
    for (int i = -1; i <= 1; ++i)
        draw->AddCircleFilled(ImVec2(center.x + static_cast<float>(i) * radius * 3.0f, center.y), radius, dotColor);
    return pressed;
}

template <typename T, typename UIFunction>
static void DrawComponents(const std::string& name, Entity entity, UIFunction function)
{
    if (!entity.HasComponent<T>())
        return;

    auto& components = entity.GetComponent<T>();
    const bool open = SectionHeader(ImGui::GetID((void*)typeid(T).hash_code()), name.c_str());
    ImGui::PushID(name.c_str());
    if (SectionMenuButton("##ComponentSettingsButton"))
        ImGui::OpenPopup("ComponentSettings");
    bool removeComponent = false;
    if (ImGui::BeginPopup("ComponentSettings"))
    {
        ImGui::TextColored(DimText(0.50f), "Component");
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Component"))
            removeComponent = true;
        ImGui::EndPopup();
    }
    if (open && BeginPropertyTable("##Properties"))
    {
        function(components);
        EndPropertyTable();
        ImGui::Spacing();
    }
    ImGui::PopID();
    if (removeComponent)
        entity.RemoveComponent<T>();
}

/**
 * @brief Body type combo shared by the 2D and 3D rigid bodies (Static, Kinematic, Dynamic).
 * @tparam Component RigidBody2DComponent or RigidBody3DComponent.
 */
template <typename Component> static void DrawBodyTypeProperty(Component& component)
{
    const char* bodyTypeStrings[] = {"Static", "Kinematic", "Dynamic"};
    const char* currentBodyTypeString = bodyTypeStrings[(int)component.Type];
    PropertyLabel("Body Type");
    if (ImGui::BeginCombo("##Body Type", currentBodyTypeString))
    {
        for (int i = 0; i < 3; i++)
        {
            bool isSelected = currentBodyTypeString == bodyTypeStrings[i];
            if (ImGui::Selectable(bodyTypeStrings[i], isSelected))
            {
                currentBodyTypeString = bodyTypeStrings[i];
                // Access via the Component Type, not the instance
                component.Type = (typename Component::BodyType)i;
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

/**
 * @brief One Add Component entry, adding \p T to \p entity when clicked.
 *
 * Greyed out rather than hidden once the entity has \p T: adding a component
 * twice trips entt's "Slot not available" assert and aborts the editor, and a
 * disabled entry still shows the component exists.
 */
template <typename T> static void AddComponentMenuItem(Entity entity, const char* label)
{
    if (ImGui::MenuItem(label, nullptr, false, !entity.HasComponent<T>()))
    {
        entity.AddComponent<T>();
        ImGui::CloseCurrentPopup();
    }
}

/// @brief Width left for a path field once \p buttonsWidth of buttons share its line.
static float PathFieldWidth(float buttonsWidth, int buttonCount)
{
    const float spacing = ImGui::GetStyle().ItemSpacing.x * static_cast<float>(buttonCount);
    return ImMax(ImGui::GetContentRegionAvail().x - buttonsWidth - spacing, 10.0f);
}

void SceneHierarchyPanel::DrawComponents(Entity entity)
{
    // Name and Add Component share one row: the name field takes what the
    // button leaves, and the button shortens its label in a narrow panel.
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* addLabel =
        ImGui::GetContentRegionAvail().x >= 260.0f ? "Add Component##AddComponentButton" : "+ Add##AddComponentButton";
    const float addWidth = ImGui::CalcTextSize(addLabel, nullptr, true).x + style.FramePadding.x * 2.0f;
    if (entity.HasComponent<TagComponent>())
    {
        auto& tag = entity.GetComponent<TagComponent>().Tag;
        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
        strncpy(buffer, tag.c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        ImGui::SetNextItemWidth(ImMax(ImGui::GetContentRegionAvail().x - addWidth - style.ItemSpacing.x, 40.0f));
        if (ImGui::InputTextWithHint("##Tag", "Name", buffer, sizeof(buffer)))
        {
            tag = std::string(buffer);
        }
        ImGui::SameLine();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentText());
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive());
    if (ImGui::Button(addLabel, ImVec2(addWidth, 0.0f)))
        ImGui::OpenPopup("AddComponents");
    ImGui::PopStyleColor(4);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Add a component to this entity");
    if (ImGui::BeginPopup("AddComponents"))
    {
        static char addFilter[128] = {};
        auto match = [&](const char* label)
        {
            if (addFilter[0] == '\0')
                return true;
            std::string haystack = label;
            std::string needle = addFilter;
            std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(needle.begin(), needle.end(), needle.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return haystack.find(needle) != std::string::npos;
        };

        ImGui::TextColored(DimText(0.50f), "ADD COMPONENT");
        EditorUI::SearchField("##AddComponentSearch", addFilter, sizeof(addFilter), "Search components...", 260.0f);
        ImGui::Separator();

        if (match("Camera"))
            AddComponentMenuItem<CameraComponent>(m_SelectionContext, "Camera");
        if (match("Sprite Renderer"))
            AddComponentMenuItem<SpriteRendererComponent>(m_SelectionContext, "Sprite Renderer");
        if (match("Text Node"))
            AddComponentMenuItem<TextComponent>(m_SelectionContext, "Text Node");
        if (match("Sprite Animation"))
            AddComponentMenuItem<SpriteAnimationComponent>(m_SelectionContext, "Sprite Animation");
        ImGui::Separator();
        if (match("Rigid Body 2D"))
            AddComponentMenuItem<RigidBody2DComponent>(m_SelectionContext, "Rigid Body 2D");
        if (match("Box Collider 2D"))
            AddComponentMenuItem<BoxColliderComponent>(m_SelectionContext, "Box Collider 2D");
        ImGui::Separator();
        if (match("Model 3D"))
            AddComponentMenuItem<Model3DComponent>(m_SelectionContext, "Model 3D");
        if (match("Rigid Body 3D"))
            AddComponentMenuItem<RigidBody3DComponent>(m_SelectionContext, "Rigid Body 3D");
        if (match("Box Collider 3D"))
            AddComponentMenuItem<BoxCollider3DComponent>(m_SelectionContext, "Box Collider 3D");
        if (match("Sphere Collider 3D"))
            AddComponentMenuItem<SphereCollider3DComponent>(m_SelectionContext, "Sphere Collider 3D");
        if (match("Capsule Collider 3D"))
            AddComponentMenuItem<CapsuleCollider3DComponent>(m_SelectionContext, "Capsule Collider 3D");
        if (match("Animator 3D"))
            AddComponentMenuItem<AnimatorComponent>(m_SelectionContext, "Animator 3D");
        if (match("Audio Source"))
            AddComponentMenuItem<AudioSourceComponent>(m_SelectionContext, "Audio Source");
        // Foliage bends a model's vertices, so it is offered only where there is a model to bend.
        if (m_SelectionContext.HasComponent<Model3DComponent>() && match("Foliage"))
            AddComponentMenuItem<FoliageComponent>(m_SelectionContext, "Foliage");
        ImGui::Separator();
        if (match("Directional Light"))
            AddComponentMenuItem<DirectionalLightComponent>(m_SelectionContext, "Directional Light");
        if (match("Point Light"))
            AddComponentMenuItem<PointLightComponent>(m_SelectionContext, "Point Light");
        if (match("Script"))
            AddComponentMenuItem<ScriptComponent>(m_SelectionContext, "Script (Lua)");
        if (match("Spot Light"))
            AddComponentMenuItem<SpotLightComponent>(m_SelectionContext, "Spot Light");
        if (match("Area Light"))
            AddComponentMenuItem<AreaLightComponent>(m_SelectionContext, "Area Light");
        if (match("Water"))
            AddComponentMenuItem<WaterComponent>(m_SelectionContext, "Water");
        if (match("Wind"))
            AddComponentMenuItem<WindComponent>(m_SelectionContext, "Wind");
        if (match("Terrain"))
            AddComponentMenuItem<TerrainComponent>(m_SelectionContext, "Terrain");
        if (match("Sequencer"))
            AddComponentMenuItem<SequencerComponent>(m_SelectionContext, "Sequencer");
        if (match("Joint 3D"))
            AddComponentMenuItem<Joint3DComponent>(m_SelectionContext, "Joint 3D");
        if (match("UI Rect"))
            AddComponentMenuItem<UIRectComponent>(m_SelectionContext, "UI Rect");
        if (match("UI Image"))
            AddComponentMenuItem<UIImageComponent>(m_SelectionContext, "UI Image");
        if (match("UI Text"))
            AddComponentMenuItem<UITextComponent>(m_SelectionContext, "UI Text");
        if (match("UI Button"))
            AddComponentMenuItem<UIButtonComponent>(m_SelectionContext, "UI Button");
        if (match("UI Progress Bar"))
            AddComponentMenuItem<UIProgressBarComponent>(m_SelectionContext, "UI Progress Bar");
        ImGui::EndPopup();
    }

    if (entity.HasComponent<Model3DComponent>() && !entity.HasComponent<ModelNodeComponent>())
    {
        auto& mc = entity.GetComponent<Model3DComponent>();
        if (mc.IsLoaded && mc.ModelData && !mc.ModelData->GetNodes().empty())
        {
            // Expand/collapse toggle. Queued, not executed here: the previous
            // direct call mutated the registry mid-draw AND ran on every click,
            // duplicating the whole node tree (the reported UI/engine weirdness).
            // A plain button in a property row: it is a secondary action of the
            // model, not the panel's primary one, so it does not take the accent.
            bool expanded = m_Context->IsModelExpanded(entity);
            const char* hint = expanded ? "One entity per glTF node" : "Fold the glTF tree into editable entities";
            if (BeginPropertyTable("##ModelNodes"))
            {
                PropertyLabel("Child Nodes", hint);
                if (ImGui::Button(expanded ? "Collapse Child Nodes" : "Expand Child Nodes", ImVec2(-FLT_MIN, 0.0f)))
                {
                    if (expanded)
                        m_Context->QueueModelCollapse(entity.GetUUID());
                    else
                        m_Context->QueueModelExpand(entity.GetUUID());
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                    ImGui::SetTooltip("%s", hint);
                EndPropertyTable();
            }
        }
    }
    ImGui::Spacing();

    // Issue #17: read-only info for entities expanded from a glTF model's node
    // tree, plus a toggle between the collapsed (single model) and expanded
    // (one entity per node) representations.
    if (entity.HasComponent<ModelNodeComponent>())
    {
        auto& mnc = entity.GetComponent<ModelNodeComponent>();
        if (SectionHeader(ImGui::GetID("##ModelNodeInfo"), "Model Node") && BeginPropertyTable("##ModelNodeProperties"))
        {
            PropertyText("glTF Node", "%s", mnc.NodeName.c_str());
            PropertyText("Node Index", "%d", mnc.NodeIndex);
            EndPropertyTable();
            ImGui::Spacing();
        }
    }

    ::UHE::DrawComponents<TransformComponent>("Transform", entity,
                                              [](auto& components)
                                              {
                                                  DrawVec3Control("Translation", components.Translation);
                                                  glm::vec3 rotation = glm::degrees(components.Rotation);
                                                  DrawVec3Control("Rotation", rotation);
                                                  components.Rotation = glm::radians(rotation);
                                                  DrawVec3Control("Scale", components.Scale, 1.0f);
                                              });

    ::UHE::DrawComponents<ScriptComponent>("Script (Lua)", entity,
                                           [](ScriptComponent& sc)
                                           {
                                               // The path resolves through the asset registry like any other asset, so
                                               // a script lives with the project's content: "Scripts/drone.lua".
                                               PropertyLabel("Script", "Project asset path, e.g. Scripts/drone.lua");
                                               ImGui::InputText("##Script", &sc.ScriptPath);
                                               PropertyLabel("Enabled");
                                               ImGui::Checkbox("##Enabled", &sc.Enabled);
                                               if (ImGui::IsItemHovered())
                                                   ImGui::SetTooltip(
                                                       "A disabled script keeps its state and stops updating.");
                                               PropertyNote("OnCreate(self)  OnUpdate(self, dt)  OnDestroy(self)");
                                               PropertyNote("File > Reload Scripts re-runs OnCreate.");
                                           });

    ::UHE::DrawComponents<TextComponent>("Text Node", entity,
                                         [](TextComponent& tc)
                                         {
                                             PropertyLabel("Color");
                                             ImGui::ColorEdit4("##Color", glm::value_ptr(tc.Color));

                                             PropertyLabel("Text");
                                             ImGui::InputTextMultiline("##Text", &tc.TextString,
                                                                       ImVec2(-FLT_MIN, 0.0f));

                                             PropertyLabel("Kerning");
                                             ImGui::DragFloat("##Kerning", &tc.Kerning, 0.025f);
                                             PropertyLabel("Line Spacing");
                                             ImGui::DragFloat("##Line Spacing", &tc.LineSpacing, 0.025f);
                                         });

    ::UHE::DrawComponents<RigidBody2DComponent>("Rigid Body 2D", entity,
                                                [](auto& components)
                                                {
                                                    DrawBodyTypeProperty(components);
                                                    PropertyLabel("Fixed Rotation");
                                                    ImGui::Checkbox("##Fixed Rotation", &components.FixedRotation);
                                                });

    ::UHE::DrawComponents<BoxColliderComponent>(
        "Box Collider 2D", entity,
        [](auto& components)
        {
            PropertyLabel("Offset");
            ImGui::DragFloat2("##Offset", glm::value_ptr(components.Offset), 0.01f);
            PropertyLabel("Size");
            ImGui::DragFloat2("##Size", glm::value_ptr(components.Size), 0.01f, 0.01f, 100.0f);
            PropertyLabel("Density");
            ImGui::DragFloat("##Density", &components.Density, 0.1f, 0.0f, 100.0f);
            PropertyLabel("Friction");
            ImGui::DragFloat("##Friction", &components.Friction, 0.01f, 0.0f, 1.0f);
            PropertyLabel("Restitution");
            ImGui::DragFloat("##Restitution", &components.Restitution, 0.01f, 0.0f, 1.0f);
            PropertyLabel("Restitution Threshold");
            ImGui::DragFloat("##Restitution Threshold", &components.RestitutionThreshold, 0.01f, 0.0f, 10.0f);
        });

    ::UHE::DrawComponents<RigidBody3DComponent>(
        "Rigid Body 3D", entity,
        [](auto& components)
        {
            DrawBodyTypeProperty(components);

            PropertyLabel("Mass");
            ImGui::DragFloat("##Mass", &components.Mass, 0.1f, 0.0f, 10000.0f);
            PropertyLabel("Linear Damping");
            ImGui::DragFloat("##Linear Damping", &components.LinearDamping, 0.01f, 0.0f, 1.0f);
            PropertyLabel("Angular Damping");
            ImGui::DragFloat("##Angular Damping", &components.AngularDamping, 0.01f, 0.0f, 1.0f);
            PropertyLabel("Is Sensor");
            ImGui::Checkbox("##Is Sensor", &components.IsSensor);

            // Allowed degrees of freedom: one row per kind, one checkbox per axis.
            PropertyLabel("Translation DOF", "Allowed degrees of freedom: translation along X, Y and Z.");
            ImGui::PushID("TranslationDOF");
            ImGui::Checkbox("X", &components.AllowedDOFs[0]);
            ImGui::SameLine();
            ImGui::Checkbox("Y", &components.AllowedDOFs[1]);
            ImGui::SameLine();
            ImGui::Checkbox("Z", &components.AllowedDOFs[2]);
            ImGui::PopID();
            PropertyLabel("Rotation DOF", "Allowed degrees of freedom: rotation about X, Y and Z.");
            ImGui::PushID("RotationDOF");
            ImGui::Checkbox("X", &components.AllowedDOFs[3]);
            ImGui::SameLine();
            ImGui::Checkbox("Y", &components.AllowedDOFs[4]);
            ImGui::SameLine();
            ImGui::Checkbox("Z", &components.AllowedDOFs[5]);
            ImGui::PopID();
        });

    ::UHE::DrawComponents<Joint3DComponent>(
        "Joint 3D", entity,
        [this](Joint3DComponent& joint)
        {
            static constexpr const char* kTypes[] = {"Fixed", "Hinge", "Slider", "Ball"};
            PropertyLabel("Type", "Fixed welds; Hinge rotates about the axis; Slider moves along it; Ball pivots.");
            int type = static_cast<int>(joint.Type);
            if (ImGui::Combo("##JointType", &type, kTypes, IM_ARRAYSIZE(kTypes)))
                joint.Type = static_cast<Joint3DComponent::JointType>(type);

            // The other body: any entity with a 3D rigid body, or the world.
            PropertyLabel("Connected To", "The other body. World pins this one in place.");
            std::string current = "World";
            if (joint.ConnectedEntity != 0)
            {
                Entity other = m_Context->GetEntityWithUUID(joint.ConnectedEntity);
                current = other ? other.GetComponent<TagComponent>().Tag : "(missing)";
            }
            if (ImGui::BeginCombo("##JointConnected", current.c_str()))
            {
                if (ImGui::Selectable("World", joint.ConnectedEntity == 0))
                    joint.ConnectedEntity = 0;
                auto bodies = m_Context->GetRegistry().view<RigidBody3DComponent, TagComponent, IDComponent>();
                for (auto handle : bodies)
                {
                    const u64 uuid = bodies.get<IDComponent>(handle).ID;
                    if (handle == static_cast<entt::entity>(m_SelectionContext))
                        continue;
                    ImGui::PushID(static_cast<int>(uuid & 0x7fffffff));
                    if (ImGui::Selectable(bodies.get<TagComponent>(handle).Tag.c_str(), joint.ConnectedEntity == uuid))
                        joint.ConnectedEntity = uuid;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }

            DrawVec3Control("Anchor", joint.Anchor);
            if (joint.Type == Joint3DComponent::JointType::Hinge || joint.Type == Joint3DComponent::JointType::Slider)
            {
                DrawVec3Control("Axis", joint.Axis);
                const bool hinge = joint.Type == Joint3DComponent::JointType::Hinge;
                PropertyLabel("Limits");
                ImGui::Checkbox("##JointLimits", &joint.LimitsEnabled);
                if (joint.LimitsEnabled)
                {
                    PropertyLabel(hinge ? "Range (deg)" : "Range (m)");
                    float range[2] = {hinge ? glm::degrees(joint.LimitMin) : joint.LimitMin,
                                      hinge ? glm::degrees(joint.LimitMax) : joint.LimitMax};
                    if (ImGui::DragFloat2("##JointRange", range, hinge ? 0.5f : 0.01f, hinge ? -180.0f : -100.0f,
                                          hinge ? 180.0f : 100.0f, "%.2f"))
                    {
                        joint.LimitMin = hinge ? glm::radians(std::min(range[0], 0.0f)) : std::min(range[0], 0.0f);
                        joint.LimitMax = hinge ? glm::radians(std::max(range[1], 0.0f)) : std::max(range[1], 0.0f);
                    }
                }
                PropertyLabel(hinge ? "Friction (N m)" : "Friction (N)");
                ImGui::DragFloat("##JointFriction", &joint.Friction, 0.01f, 0.0f, 10000.0f);

                static constexpr const char* kMotors[] = {"Off", "Velocity", "Position"};
                PropertyLabel("Motor", "Scripts drive it with SetJointMotor; read back with GetJointPosition.");
                int motor = static_cast<int>(joint.Motor);
                if (ImGui::Combo("##JointMotor", &motor, kMotors, IM_ARRAYSIZE(kMotors)))
                    joint.Motor = static_cast<Joint3DComponent::MotorMode>(motor);
                if (joint.Motor != Joint3DComponent::MotorMode::Off)
                {
                    const bool velocity = joint.Motor == Joint3DComponent::MotorMode::Velocity;
                    PropertyLabel(velocity ? (hinge ? "Target (rad/s)" : "Target (m/s)")
                                           : (hinge ? "Target (rad)" : "Target (m)"));
                    ImGui::DragFloat("##JointTarget", &joint.MotorTarget, 0.01f);
                    PropertyLabel(hinge ? "Max Torque" : "Max Force");
                    ImGui::DragFloat("##JointMaxForce", &joint.MotorMaxForce, 0.5f, 0.0f, 1e6f);
                    if (!velocity)
                    {
                        PropertyLabel("Stiffness (Hz)");
                        ImGui::DragFloat("##JointFrequency", &joint.MotorFrequency, 0.05f, 0.0f, 100.0f);
                        PropertyLabel("Damping");
                        ImGui::DragFloat("##JointDamping", &joint.MotorDamping, 0.01f, 0.0f, 10.0f);
                    }
                }
                if (joint.RuntimeConstraint != nullptr)
                {
                    // In Play: the encoder, read the way a script reads it.
                    PropertyLabel("Encoder");
                    if (hinge)
                        ImGui::Text("%.1f deg, %.2f rad/s", glm::degrees(joint.RuntimePosition), joint.RuntimeVelocity);
                    else
                        ImGui::Text("%.3f m, %.3f m/s", joint.RuntimePosition, joint.RuntimeVelocity);
                }
            }
        });

    ::UHE::DrawComponents<BoxCollider3DComponent>(
        "Box Collider 3D", entity,
        [](auto& components)
        {
            DrawVec3Control("Offset", components.Offset);
            DrawVec3Control("Half Extent", components.HalfExtent);
            PropertyLabel("Friction");
            ImGui::DragFloat("##Friction", &components.Friction, 0.01f, 0.0f, 1.0f);
            PropertyLabel("Restitution");
            ImGui::DragFloat("##Restitution", &components.Restitution, 0.01f, 0.0f, 1.0f);
        });

    ::UHE::DrawComponents<SphereCollider3DComponent>(
        "Sphere Collider 3D", entity,
        [](auto& components)
        {
            DrawVec3Control("Offset", components.Offset);
            PropertyLabel("Radius");
            ImGui::DragFloat("##Radius", &components.Radius, 0.01f, 0.0f, 100.0f);
            PropertyLabel("Friction");
            ImGui::DragFloat("##Friction", &components.Friction, 0.01f, 0.0f, 1.0f);
            PropertyLabel("Restitution");
            ImGui::DragFloat("##Restitution", &components.Restitution, 0.01f, 0.0f, 1.0f);
        });

    ::UHE::DrawComponents<CapsuleCollider3DComponent>(
        "Capsule Collider 3D", entity,
        [](auto& components)
        {
            DrawVec3Control("Offset", components.Offset);
            PropertyLabel("Radius");
            ImGui::DragFloat("##Radius", &components.Radius, 0.01f, 0.0f, 100.0f);
            PropertyLabel("Half Height");
            ImGui::DragFloat("##Half Height", &components.HalfHeight, 0.01f, 0.0f, 100.0f);
            PropertyLabel("Friction");
            ImGui::DragFloat("##Friction", &components.Friction, 0.01f, 0.0f, 1.0f);
            PropertyLabel("Restitution");
            ImGui::DragFloat("##Restitution", &components.Restitution, 0.01f, 0.0f, 1.0f);
        });

    ::UHE::DrawComponents<CameraComponent>(
        "Camera", entity,
        [](auto& components)
        {
            auto& camera = components.Camera;
            PropertyLabel("Primary");
            ImGui::Checkbox("##Primary", &components.Primary);
            const char* projectionTypeString[] = {"Projection", "Orthographic"};
            const char* currnetProjectionTypeString = projectionTypeString[(int)(camera).GetProjectionType()];
            PropertyLabel("Projection");
            if (ImGui::BeginCombo("##Projection", currnetProjectionTypeString))
            {
                for (int i{0}; i < 2; i++)
                {
                    bool isSelected = currnetProjectionTypeString == projectionTypeString[i];
                    if (ImGui::Selectable(projectionTypeString[i], isSelected))
                    {
                        currnetProjectionTypeString = projectionTypeString[i];
                        camera.SetProjectionType((SceneCamera::ProjectionType)i);
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (camera.GetProjectionType() == SceneCamera::ProjectionType::Perpective)
            {
                f32 ProjFOV = glm::degrees(camera.GetProjectionFOV());
                PropertyLabel("FOV");
                if (ImGui::DragFloat("##FOV", &ProjFOV))
                {
                    camera.SetPerspectiveFOV(glm::radians(ProjFOV));
                }
                f32 ProjNear = camera.GetProjectionNear();
                PropertyLabel("FOV Near");
                if (ImGui::DragFloat("##FOV Near", &ProjNear))
                {
                    camera.SetPerpecstiveNear(ProjNear);
                }
                f32 ProjFar = camera.GetProjectionFar();
                PropertyLabel("FOV Far");
                if (ImGui::DragFloat("##FOV Far", &ProjFar))
                {
                    camera.SetPerspecstiveFar(ProjFar);
                }
            }
            if (camera.GetProjectionType() == SceneCamera::ProjectionType::Orhtographic)
            {
                f32 orthoSize = camera.GetOrthographicSize();
                PropertyLabel("Size");
                if (ImGui::DragFloat("##Size", &orthoSize))
                {
                    camera.SetOrthographicSize(orthoSize);
                }
                f32 orthoNear = camera.GetOrthographicNearClip();
                PropertyLabel("Near");
                if (ImGui::DragFloat("##Near", &orthoNear))
                {
                    camera.SetOrthoGraphicNearClip(orthoNear);
                }
                f32 orthoFar = camera.GetOrthographicFarClip();
                PropertyLabel("Far");
                if (ImGui::DragFloat("##Far", &orthoFar))
                {
                    camera.SetOrthoGraphicFarClip(orthoFar);
                }
                PropertyLabel("Fixed Aspect Ratio");
                ImGui::Checkbox("##Fixed aspect ratio", &components.FixedAspectRatio);
            }
        });
    ::UHE::DrawComponents<SpriteRendererComponent>(
        "Sprite Renderer", entity,
        [](SpriteRendererComponent& src)
        {
            PropertyLabel("Color");
            ImGui::ColorEdit4("##Color", glm::value_ptr(src.Color));

            char texBuf[256];
            memset(texBuf, 0, sizeof(texBuf));
            strncpy(texBuf, src.TexturePath.c_str(), sizeof(texBuf) - 1);
            texBuf[sizeof(texBuf) - 1] = '\0';
            PropertyLabel("Texture Path");
            ImGui::SetNextItemWidth(PathFieldWidth(kBrowseButtonWidth, 1));
            if (ImGui::InputText("##TextureInput", texBuf, sizeof(texBuf)))
            {
                src.TexturePath = std::string(texBuf);
                src.Texture = nullptr;
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse##Tex", ImVec2(kBrowseButtonWidth, 0)))
            {
                std::string path = FileDialogs::OpenFile("Image (*.png;*.jpg)\0*.png;*.jpg\0");
                if (!path.empty())
                {
                    src.TexturePath = path;
                    src.Texture = Texture2D::Create(path);
                }
            }

            PropertyLabel("Texture", "Drop a .png or .jpg from the Content Browser here.");
            if (src.Texture)
            {
                ImGui::Image((ImTextureID)(intptr_t)src.Texture->GetImGuiTextureID(), ImVec2(100.0f, 100.0f),
                             ImVec2(0, 1), ImVec2(1, 0));
            }
            else
            {
                ImGui::Button("Drop Texture Here", ImVec2(-FLT_MIN, kDropZoneHeight));
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("Content_Browser_item"))
                {
                    const char* path = (const char*)payload->Data;
                    std::filesystem::path itemPath(path);
                    UHE_CORE_INFO("Accepted DragDropPayload: {0}", path);
                    if (itemPath.extension() == ".png" || itemPath.extension() == ".jpg")
                    {
                        src.TexturePath = path;
                        src.Texture = Texture2D::Create(path);
                        if (src.Texture)
                        {
                            UHE_CORE_INFO("Successfully loaded texture from payload!");
                        }
                        else
                        {
                            UHE_CORE_ERROR("Failed to load texture from payload!");
                        }
                    }
                    else
                    {
                        UHE_CORE_WARN("Payload extension not supported: {0}", itemPath.extension().string());
                    }
                }
                ImGui::EndDragDropTarget();
            }

            PropertyLabel("Tiling");
            ImGui::DragFloat("##Tiling", &src.TilingFactor, 0.1f, 0.0f, 100.0f);
            PropertyLabel("Use SubTexture");
            ImGui::Checkbox("##Use SubTexture", &src.UseSubTexture);
            if (src.UseSubTexture)
            {
                PropertyLabel("Coords");
                ImGui::DragFloat2("##Coords", glm::value_ptr(src.SubTextureCoords), 1.0f);
                PropertyLabel("Cell Size");
                ImGui::DragFloat2("##Cell Size", glm::value_ptr(src.SubTextureCellSize), 1.0f);
                PropertyLabel("Sprite Size");
                ImGui::DragFloat2("##Sprite Size", glm::value_ptr(src.SubTextureSpriteSize), 1.0f);
                if (src.Texture)
                {
                    float texWidth = static_cast<float>(src.Texture->GetWidth());
                    float texHeight = static_cast<float>(src.Texture->GetHeight());
                    // SubTexture2D::CreateFromCoords math
                    glm::vec2 min = {(src.SubTextureCoords.x * src.SubTextureCellSize.x) / texWidth,
                                     (src.SubTextureCoords.y * src.SubTextureCellSize.y) / texHeight};
                    glm::vec2 max = {
                        ((src.SubTextureCoords.x + src.SubTextureSpriteSize.x) * src.SubTextureCellSize.x) / texWidth,
                        ((src.SubTextureCoords.y + src.SubTextureSpriteSize.y) * src.SubTextureCellSize.y) / texHeight};
                    PropertyLabel("SubTexture Preview");
                    ImGui::Image((ImTextureID)src.Texture->GetImGuiTextureID(), ImVec2(64, 64), ImVec2(min.x, max.y),
                                 ImVec2(max.x, min.y));
                }
            }
            else if (src.Texture)
            {
                PropertyLabel("Texture Preview");
                ImGui::Image((ImTextureID)src.Texture->GetImGuiTextureID(), ImVec2(64, 64), ImVec2(0, 1), ImVec2(1, 0));
            }
        });
    ::UHE::DrawComponents<SpriteAnimationComponent>(
        "Sprite Animation", entity,
        [](SpriteAnimationComponent& animComp)
        {
            char sheetBuf[256];
            memset(sheetBuf, 0, sizeof(sheetBuf));
            strncpy(sheetBuf, animComp.SpriteSheetPath.c_str(), sizeof(sheetBuf) - 1);
            sheetBuf[sizeof(sheetBuf) - 1] = '\0';
            PropertyLabel("SpriteSheet");
            ImGui::SetNextItemWidth(PathFieldWidth(kBrowseButtonWidth, 1));
            if (ImGui::InputText("##SpritesheetInput", sheetBuf, sizeof(sheetBuf)))
            {
                animComp.SpriteSheetPath = std::string(sheetBuf);
                animComp.Animation.SpriteSheet = nullptr;
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse##Anim", ImVec2(kBrowseButtonWidth, 0)))
            {
                std::string path = FileDialogs::OpenFile("Image (*.png;*.jpg)\0*.png;*.jpg\0");
                if (!path.empty())
                {
                    animComp.SpriteSheetPath = path;
                    animComp.Animation.SpriteSheet = Texture2D::Create(path);
                }
            }

            // Spritesheet preview
            PropertyLabel("Atlas Preview", "Drop a .png or .jpg from the Content Browser here.");
            if (animComp.Animation.SpriteSheet)
            {
                ImGui::Image((ImTextureID)(intptr_t)animComp.Animation.SpriteSheet->GetImGuiTextureID(), ImVec2(64, 64),
                             ImVec2(0, 1), ImVec2(1, 0));
            }
            else
            {
                ImGui::Button("Drop Texture Here##Anim", ImVec2(-FLT_MIN, kDropZoneHeight));
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("Content_Browser_item"))
                {
                    const char* path = (const char*)payload->Data;
                    std::filesystem::path itemPath(path);
                    UHE_CORE_INFO("Accepted DragDropPayload (Anim): {0}", path);
                    if (itemPath.extension() == ".png" || itemPath.extension() == ".jpg")
                    {
                        animComp.SpriteSheetPath = path;
                        animComp.Animation.SpriteSheet = Texture2D::Create(path);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            PropertyLabel("Frame Size");
            ImGui::DragFloat2("##Frame Size", glm::value_ptr(animComp.Animation.FrameSize), 1.0f, 1.0f, 4096.0f);
            PropertyLabel("Frame Count");
            ImGui::DragInt("##Frame Count", &animComp.Animation.FrameCount, 1, 1, 1000);
            PropertyLabel("Frame Duration");
            ImGui::DragFloat("##Frame Duration", &animComp.Animation.FrameDuration, 0.01f, 0.001f, 10.0f, "%.3f s");
            PropertyLabel("Loop");
            ImGui::Checkbox("##Loop", &animComp.Animation.Loop);
            PropertyLabel("Color");
            ImGui::ColorEdit4("##Color", glm::value_ptr(animComp.Color));

            PropertyLabel("Playback");
            if (ImGui::Button(animComp.Animation.Playing ? "Pause" : "Play"))
                animComp.Animation.Playing = !animComp.Animation.Playing;
            ImGui::SameLine();
            if (ImGui::Button("Reset"))
            {
                animComp.Animation.CurrentFrameIndex = 0;
                animComp.Animation.Timer = 0.0f;
            }
            PropertyText("Frame", "%d / %d", animComp.Animation.CurrentFrameIndex, animComp.Animation.FrameCount);
        });

    ::UHE::DrawComponents<Model3DComponent>(
        "3D Model", entity,
        [](Model3DComponent& mc)
        {
            char pathBuf[256];
            memset(pathBuf, 0, sizeof(pathBuf));
            strncpy(pathBuf, mc.ModelPath.c_str(), sizeof(pathBuf) - 1);
            pathBuf[sizeof(pathBuf) - 1] = '\0';
            const float loadWidth = ImGui::CalcTextSize("Load").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            PropertyLabel("Model");
            ImGui::SetNextItemWidth(PathFieldWidth(loadWidth + kBrowseButtonWidth, 2));
            if (ImGui::InputText("##ModelInput", pathBuf, sizeof(pathBuf)))
            {
                mc.ModelPath = std::string(pathBuf);
            }
            ImGui::SameLine();
            if (ImGui::Button("Load", ImVec2(loadWidth, 0)))
            {
                mc.IsLoaded = mc.ModelData->loadModel(mc.ModelPath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse##Model", ImVec2(kBrowseButtonWidth, 0)))
            {
                std::string path = FileDialogs::OpenFile("GLTF/GLB (*.gltf;*.glb)\0*.gltf;*.glb\0");
                if (!path.empty())
                {
                    mc.ModelPath = path;
                    mc.IsLoaded = mc.ModelData->loadModel(path);
                }
            }

            PropertyLabel("Status", "Drop a .gltf or .glb from the Content Browser here.");
            if (mc.IsLoaded)
            {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(EditorTheme::Success(), "Model Loaded Successfully");
            }
            else
            {
                ImGui::Button("Drop 3D Model Here", ImVec2(-FLT_MIN, kDropZoneHeight));
            }

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("Content_Browser_item"))
                {
                    const char* path = (const char*)payload->Data;
                    std::filesystem::path itemPath(path);
                    UHE_CORE_INFO("Accepted DragDropPayload (3D Model): {0}", path);
                    if (itemPath.extension() == ".gltf" || itemPath.extension() == ".glb")
                    {
                        mc.ModelPath = path;
                        mc.IsLoaded = mc.ModelData->loadModel(path);
                        if (mc.IsLoaded)
                        {
                            UHE_CORE_INFO("Successfully loaded 3D model from payload!");
                        }
                        else
                        {
                            UHE_CORE_ERROR("Failed to load 3D model from payload!");
                        }
                    }
                    else
                    {
                        UHE_CORE_WARN("Payload extension not supported: {0}", itemPath.extension().string());
                    }
                }
                ImGui::EndDragDropTarget();
            }
        });

    ::UHE::DrawComponents<DirectionalLightComponent>(
        "Directional Light", entity,
        [](DirectionalLightComponent& light)
        {
            PropertyLabel("Color");
            ImGui::ColorEdit3("##Color", glm::value_ptr(light.Color));
            PropertyLabel("Intensity");
            ImGui::DragFloat("##Intensity", &light.Intensity, 0.1f, 0.0f, 100.0f);
            PropertyLabel("Cast Shadows", "The first directional light with this set casts the scene's shadows. "
                                          "Tune them in Environment > Shadows.");
            ImGui::Checkbox("##Cast Shadows", &light.CastShadows);
            PropertyNote("Rotate the entity's Transform to change light direction.");
        });

    ::UHE::DrawComponents<FoliageComponent>(
        "Foliage", entity,
        [entity](FoliageComponent& foliage)
        {
            static constexpr const char* kWindHint = "Sways in the scene's wind; see Environment > Wind";
            PropertyLabel("Bend", kWindHint);
            ImGui::SliderFloat("##Bend", &foliage.Bend, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip("%s", kWindHint);
            PropertyLabel("Height", "Model-space height where the bend reaches full strength; the base stays put.");
            ImGui::DragFloat("##Height", &foliage.Height, 0.05f, 0.1f, 50.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            UHE::Entity owner = entity;
            if (!owner.HasComponent<Model3DComponent>())
                PropertyNote("Requires a 3D Model on the entity.");
        });

    ::UHE::DrawComponents<PointLightComponent>(
        "Point Light", entity,
        [](PointLightComponent& light)
        {
            PropertyLabel("Color");
            ImGui::ColorEdit3("##Color", glm::value_ptr(light.Color));
            PropertyLabel("Intensity");
            ImGui::DragFloat("##Intensity", &light.Intensity, 0.1f, 0.0f, 100.0f);
            PropertyLabel("Radius");
            ImGui::DragFloat("##Radius", &light.Radius, 0.5f, 0.0f, 1000.0f);
            PropertyLabel("Cast Shadows", "Six shadow tiles (one per cube face): the costliest "
                                          "light to shadow.");
            ImGui::Checkbox("##Cast Shadows", &light.CastShadows);
        });

    ::UHE::DrawComponents<SpotLightComponent>(
        "Spot Light", entity,
        [](SpotLightComponent& light)
        {
            PropertyLabel("Color");
            ImGui::ColorEdit3("##Color", glm::value_ptr(light.Color));
            PropertyLabel("Intensity");
            ImGui::DragFloat("##Intensity", &light.Intensity, 0.1f, 0.0f, 100.0f);
            PropertyLabel("Radius");
            ImGui::DragFloat("##Radius", &light.Radius, 0.5f, 0.0f, 1000.0f);
            // The component stores radians (glTF KHR_lights_punctual parity); the
            // panel edits degrees like every other angle in the editor.
            float innerDeg = glm::degrees(light.InnerConeAngle);
            float outerDeg = glm::degrees(light.OuterConeAngle);
            PropertyLabel("Inner Cone (deg)");
            if (ImGui::DragFloat("##Inner Cone (deg)", &innerDeg, 0.5f, 0.0f, 89.0f))
                light.InnerConeAngle = glm::radians(glm::clamp(innerDeg, 0.0f, outerDeg));
            PropertyLabel("Outer Cone (deg)");
            if (ImGui::DragFloat("##Outer Cone (deg)", &outerDeg, 0.5f, 0.0f, 89.0f))
                light.OuterConeAngle = glm::radians(glm::max(outerDeg, glm::degrees(light.InnerConeAngle)));
            PropertyLabel("Cast Shadows", "One shadow tile.");
            ImGui::Checkbox("##Cast Shadows", &light.CastShadows);
            PropertyNote("Rotate the entity's Transform to aim the cone.");
        });

    ::UHE::DrawComponents<AreaLightComponent>(
        "Area Light", entity,
        [](AreaLightComponent& light)
        {
            PropertyLabel("Color");
            ImGui::ColorEdit3("##Color", glm::value_ptr(light.Color));
            PropertyLabel("Intensity");
            ImGui::DragFloat("##Intensity", &light.Intensity, 0.1f, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            // A zero-area panel has no form factor, so the size stops short of 0.
            PropertyLabel("Width", "Panel size along the entity's local X.");
            ImGui::DragFloat("##Width", &light.Width, 0.05f, 0.01f, 50.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Height", "Panel size along the entity's local Y.");
            ImGui::DragFloat("##Height", &light.Height, 0.05f, 0.01f, 50.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Range", "Distance at which the light fades out.");
            ImGui::DragFloat("##Range", &light.Range, 0.1f, 0.1f, 200.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Two Sided", "Also light the side behind the panel.");
            ImGui::Checkbox("##Two Sided", &light.TwoSided);
            PropertyLabel("Cast Shadows", "One shadow tile, cast from the panel's centre.");
            ImGui::Checkbox("##Cast Shadows", &light.CastShadows);
            PropertyNote("Shines along the entity's local -Z; rotate the Transform to aim the panel.");
        });

    ::UHE::DrawComponents<WaterComponent>(
        "Water", entity,
        [](WaterComponent& water)
        {
            PropertyLabel("Size", "Extent on the entity's local X and Z, in metres.");
            ImGui::DragFloat2("##Size", glm::value_ptr(water.Size), 0.1f, 0.1f, 10000.0f, "%.1f",
                              ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Resolution", "Grid cells per side: vertex detail of the waves.");
            ImGui::DragInt("##Resolution", &water.Resolution, 1.0f, 8, 512, "%d", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Shallow Color");
            ImGui::ColorEdit3("##Shallow Color", glm::value_ptr(water.ShallowColor));
            PropertyLabel("Deep Color");
            ImGui::ColorEdit3("##Deep Color", glm::value_ptr(water.DeepColor));
            PropertyLabel("Clarity", "Metres of water before the bottom fades into the deep colour.");
            ImGui::DragFloat("##Clarity", &water.Clarity, 0.05f, 0.1f, 50.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Wave Height", "Amplitude of the largest wave, in metres.");
            ImGui::DragFloat("##Wave Height", &water.WaveHeight, 0.01f, 0.0f, 3.0f, "%.2f",
                             ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Wave Length", "Wavelength of the largest wave, in metres.");
            ImGui::DragFloat("##Wave Length", &water.WaveLength, 0.1f, 0.5f, 100.0f, "%.1f",
                             ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Wave Speed", "Multiplies the physical (deep water) wave speed.");
            ImGui::DragFloat("##Wave Speed", &water.WaveSpeed, 0.01f, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Choppiness", "0 = rolling swell, 1 = sharp crests.");
            ImGui::SliderFloat("##Choppiness", &water.Choppiness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Follow Wind", "Waves travel with the scene's wind; see Environment > Wind.");
            ImGui::Checkbox("##Follow Wind", &water.FollowWind);
            // The wind supplies the direction while followed, so the field is shown but not editable.
            PropertyLabel("Direction (deg)", "Travel direction on the ground plane, used when not following the wind.");
            ImGui::BeginDisabled(water.FollowWind);
            ImGui::SliderFloat("##Direction (deg)", &water.Direction, 0.0f, 360.0f, "%.0f",
                               ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
            PropertyLabel("Foam", "Shoreline and crest foam.");
            ImGui::SliderFloat("##Foam", &water.Foam, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        });

    ::UHE::DrawComponents<UIRectComponent>(
        "UI Rect", entity,
        [](UIRectComponent& rect)
        {
            // Presets for the anchors people use most; the fields stay editable.
            struct Preset
            {
                const char* Name;
                glm::vec2 Min, Max, Pivot;
            };
            static const Preset kPresets[] = {
                {"Top Left", {0, 0}, {0, 0}, {0, 0}},
                {"Top", {0.5f, 0}, {0.5f, 0}, {0.5f, 0}},
                {"Top Right", {1, 0}, {1, 0}, {1, 0}},
                {"Left", {0, 0.5f}, {0, 0.5f}, {0, 0.5f}},
                {"Center", {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}},
                {"Right", {1, 0.5f}, {1, 0.5f}, {1, 0.5f}},
                {"Bottom Left", {0, 1}, {0, 1}, {0, 1}},
                {"Bottom", {0.5f, 1}, {0.5f, 1}, {0.5f, 1}},
                {"Bottom Right", {1, 1}, {1, 1}, {1, 1}},
                {"Stretch", {0, 0}, {1, 1}, {0.5f, 0.5f}},
                {"Stretch Top", {0, 0}, {1, 0}, {0.5f, 0}},
                {"Stretch Bottom", {0, 1}, {1, 1}, {0.5f, 1}},
            };
            PropertyLabel("Anchor Preset", "Pins the element to a point or edge of its parent (or the screen).");
            if (ImGui::BeginCombo("##AnchorPreset", "Choose..."))
            {
                for (const Preset& preset : kPresets)
                {
                    if (ImGui::Selectable(preset.Name))
                    {
                        rect.AnchorMin = preset.Min;
                        rect.AnchorMax = preset.Max;
                        rect.Pivot = preset.Pivot;
                        rect.Position = glm::vec2(0.0f);
                        // Stretched axes start with no margin.
                        if (preset.Min.x != preset.Max.x)
                            rect.Size.x = 0.0f;
                        if (preset.Min.y != preset.Max.y)
                            rect.Size.y = 0.0f;
                    }
                }
                ImGui::EndCombo();
            }
            PropertyLabel("Position", "Pixels from the anchor to the pivot (y down).");
            ImGui::DragFloat2("##UIPosition", glm::value_ptr(rect.Position), 1.0f);
            PropertyLabel("Size", "Pixels; on a stretched axis, added to the stretched size (negative = margins).");
            ImGui::DragFloat2("##UISize", glm::value_ptr(rect.Size), 1.0f);
            PropertyLabel("Anchor Min", "Fractions of the parent: (0,0) top left, (1,1) bottom right.");
            ImGui::DragFloat2("##AnchorMin", glm::value_ptr(rect.AnchorMin), 0.01f, 0.0f, 1.0f);
            PropertyLabel("Anchor Max");
            ImGui::DragFloat2("##AnchorMax", glm::value_ptr(rect.AnchorMax), 0.01f, 0.0f, 1.0f);
            PropertyLabel("Pivot", "The point of the element that sits at Position.");
            ImGui::DragFloat2("##Pivot", glm::value_ptr(rect.Pivot), 0.01f, 0.0f, 1.0f);
            PropertyLabel("Order", "Among siblings: higher draws on top and gets the mouse first.");
            ImGui::DragInt("##UIOrder", &rect.Order);
            PropertyLabel("Visible");
            ImGui::Checkbox("##UIVisible", &rect.Visible);
        });

    ::UHE::DrawComponents<UIImageComponent>(
        "UI Image", entity,
        [](UIImageComponent& image)
        {
            PropertyLabel("Color", "Multiplies the image; a button replaces it with its state colours.");
            ImGui::ColorEdit4("##UIImageColor", glm::value_ptr(image.Color));
            PropertyLabel("Image", "A texture in the project's assets; empty for a plain box. Drop one here.");
            char path[512];
            std::snprintf(path, sizeof(path), "%s", image.TexturePath.c_str());
            if (ImGui::InputTextWithHint("##UIImagePath", "none", path, sizeof(path),
                                         ImGuiInputTextFlags_EnterReturnsTrue))
                image.TexturePath = path;
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("Content_Browser_item"))
                {
                    const std::filesystem::path dropped(static_cast<const char*>(payload->Data));
                    std::string stored = dropped.generic_string();
                    std::error_code ec;
                    for (const auto& [scheme, root] : Vfs::Get().GetMounts())
                    {
                        const std::filesystem::path relative =
                            std::filesystem::weakly_canonical(dropped, ec)
                                .lexically_relative(std::filesystem::weakly_canonical(root, ec));
                        if (!relative.empty() && *relative.begin() != "..")
                        {
                            stored = relative.generic_string();
                            break;
                        }
                    }
                    image.TexturePath = stored;
                }
                ImGui::EndDragDropTarget();
            }
            PropertyLabel("Corner Radius", "Pixels.");
            ImGui::DragFloat("##UICorner", &image.CornerRadius, 0.25f, 0.0f, 500.0f, "%.1f",
                             ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Border Width", "Pixels; 0 for none.");
            ImGui::DragFloat("##UIBorder", &image.BorderWidth, 0.1f, 0.0f, 50.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Border Color");
            ImGui::ColorEdit4("##UIBorderColor", glm::value_ptr(image.BorderColor));
        });

    ::UHE::DrawComponents<UITextComponent>(
        "UI Text", entity,
        [](UITextComponent& text)
        {
            PropertyLabel("Text", "Several lines: press Enter for a new line.");
            char buffer[1024];
            std::snprintf(buffer, sizeof(buffer), "%s", text.Text.c_str());
            if (ImGui::InputTextMultiline("##UIText", buffer, sizeof(buffer), ImVec2(-1.0f, 54.0f)))
                text.Text = buffer;
            PropertyLabel("Font Size", "Pixels per line.");
            ImGui::DragFloat("##UIFontSize", &text.FontSize, 0.25f, 4.0f, 400.0f, "%.1f px",
                             ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Color");
            ImGui::ColorEdit4("##UITextColor", glm::value_ptr(text.Color));
            const char* alignments[] = {"Left", "Center", "Right"};
            int align = static_cast<int>(text.Align);
            PropertyLabel("Align");
            if (ImGui::Combo("##UIAlign", &align, alignments, 3))
                text.Align = static_cast<UITextComponent::Alignment>(align);
            PropertyLabel("Font", "A .ttf in the assets; empty for the engine's font.");
            char font[512];
            std::snprintf(font, sizeof(font), "%s", text.FontPath.c_str());
            if (ImGui::InputTextWithHint("##UIFont", "default", font, sizeof(font),
                                         ImGuiInputTextFlags_EnterReturnsTrue))
                text.FontPath = font;
        });

    ::UHE::DrawComponents<UIButtonComponent>("UI Button", entity,
                                             [](UIButtonComponent& button)
                                             {
                                                 PropertyLabel("Normal");
                                                 ImGui::ColorEdit4("##UINormal", glm::value_ptr(button.NormalColor));
                                                 PropertyLabel("Hover");
                                                 ImGui::ColorEdit4("##UIHover", glm::value_ptr(button.HoverColor));
                                                 PropertyLabel("Pressed");
                                                 ImGui::ColorEdit4("##UIPressed", glm::value_ptr(button.PressedColor));
                                                 PropertyLabel("Interactable");
                                                 ImGui::Checkbox("##UIInteractable", &button.Interactable);
                                                 PropertyNote("A click calls OnClick(self) in this entity's script.");
                                             });

    ::UHE::DrawComponents<UIProgressBarComponent>(
        "UI Progress Bar", entity,
        [](UIProgressBarComponent& bar)
        {
            PropertyLabel("Value", "0..1; SetValue in a script.");
            ImGui::SliderFloat("##UIValue", &bar.Value, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Fill");
            ImGui::ColorEdit4("##UIFill", glm::value_ptr(bar.FillColor));
            PropertyLabel("Background");
            ImGui::ColorEdit4("##UIBarBackground", glm::value_ptr(bar.BackgroundColor));
            PropertyLabel("Corner Radius", "Pixels.");
            ImGui::DragFloat("##UIBarCorner", &bar.CornerRadius, 0.25f, 0.0f, 100.0f, "%.1f",
                             ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Slider", "The mouse drags the value in Play.");
            ImGui::Checkbox("##UISlider", &bar.Interactable);
        });

    ::UHE::DrawComponents<TerrainComponent>(
        "Terrain", entity,
        [](TerrainComponent& terrain)
        {
            PropertyLabel("Heightmap", "A grayscale image in the project's assets: white is Height. Empty: "
                                       "generated noise. Drop an image here.");
            char path[512];
            std::snprintf(path, sizeof(path), "%s", terrain.HeightmapPath.c_str());
            if (ImGui::InputTextWithHint("##Heightmap", "generated", path, sizeof(path),
                                         ImGuiInputTextFlags_EnterReturnsTrue))
                terrain.HeightmapPath = path;
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("Content_Browser_item"))
                {
                    // Stored relative to the mount it is under, as scenes store paths.
                    const std::filesystem::path dropped(static_cast<const char*>(payload->Data));
                    std::string stored = dropped.generic_string();
                    std::error_code ec;
                    for (const auto& [scheme, root] : Vfs::Get().GetMounts())
                    {
                        const std::filesystem::path relative =
                            std::filesystem::weakly_canonical(dropped, ec)
                                .lexically_relative(std::filesystem::weakly_canonical(root, ec));
                        if (!relative.empty() && *relative.begin() != "..")
                        {
                            stored = relative.generic_string();
                            break;
                        }
                    }
                    terrain.HeightmapPath = stored;
                }
                ImGui::EndDragDropTarget();
            }
            PropertyLabel("Size", "Width and depth in metres, centred on the entity.");
            ImGui::DragFloat("##TerrainSize", &terrain.Size, 0.5f, 1.0f, 10000.0f, "%.1f m",
                             ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Height", "Metres between the lowest and the highest point.");
            ImGui::DragFloat("##TerrainHeight", &terrain.Height, 0.1f, 0.0f, 2000.0f, "%.1f m",
                             ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Resolution", "Quads per side. Higher is smoother and slower to rebuild.");
            ImGui::DragInt("##TerrainResolution", &terrain.Resolution, 1.0f, 8, 1024, "%d",
                           ImGuiSliderFlags_AlwaysClamp);

            ImGui::BeginDisabled(!terrain.HeightmapPath.empty());
            PropertyLabel("Seed", "Another seed, other hills.");
            ImGui::DragInt("##TerrainSeed", &terrain.Seed, 1.0f);
            PropertyLabel("Feature Size", "Metres across the largest hills.");
            ImGui::DragFloat("##FeatureSize", &terrain.FeatureSize, 0.2f, 1.0f, 2000.0f, "%.1f m",
                             ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Octaves", "Layers of finer detail.");
            ImGui::SliderInt("##Octaves", &terrain.Octaves, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Roughness", "How much each finer layer adds.");
            ImGui::SliderFloat("##Roughness", &terrain.Roughness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();

            PropertyLabel("Sand");
            ImGui::ColorEdit3("##Sand", glm::value_ptr(terrain.SandColor));
            PropertyLabel("Grass");
            ImGui::ColorEdit3("##Grass", glm::value_ptr(terrain.GrassColor));
            PropertyLabel("Rock");
            ImGui::ColorEdit3("##Rock", glm::value_ptr(terrain.RockColor));
            PropertyLabel("Snow");
            ImGui::ColorEdit3("##Snow", glm::value_ptr(terrain.SnowColor));
            PropertyLabel("Sand Level", "Below this fraction of the height: sand.");
            ImGui::SliderFloat("##SandLevel", &terrain.SandLevel, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Snow Level", "Above this fraction of the height: snow.");
            ImGui::SliderFloat("##SnowLevel", &terrain.SnowLevel, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Rock Slope", "Steeper than this: rock.");
            ImGui::SliderFloat("##RockSlope", &terrain.RockSlope, 1.0f, 89.0f, "%.0f\xC2\xB0",
                               ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Collider", "A static heightfield body in Play: bodies rest and drive on it.");
            ImGui::Checkbox("##TerrainCollider", &terrain.Collider);
        });

    ::UHE::DrawComponents<SequencerComponent>(
        "Sequencer", entity,
        [](SequencerComponent& sequencer)
        {
            PropertyLabel("Length", "Seconds; playback ends (or loops) here.");
            ImGui::DragFloat("##SeqLength", &sequencer.Data.Duration, 0.05f, 0.1f, 3600.0f, "%.2f s",
                             ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Loop", "Start over at the end instead of stopping.");
            ImGui::Checkbox("##SeqLoop", &sequencer.Data.Loop);
            PropertyLabel("Play on Start", "Plays from 0 when Play starts; otherwise a script calls PlaySequence.");
            ImGui::Checkbox("##SeqPlayOnStart", &sequencer.PlayOnStart);
            PropertyLabel("Speed", "Playback rate; negative plays backwards without events.");
            ImGui::DragFloat("##SeqSpeed", &sequencer.Speed, 0.01f, -4.0f, 4.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
            std::size_t keys = 0;
            for (const SequenceTrack& track : sequencer.Data.Tracks)
                keys += track.Keys.size();
            ImGui::TextDisabled("%zu tracks, %zu keys. Edit them in View > Sequencer.", sequencer.Data.Tracks.size(),
                                keys);
        });

    ::UHE::DrawComponents<WindComponent>(
        "Wind", entity,
        [](WindComponent& wind)
        {
            PropertyLabel("Strength", "Steady wind.");
            ImGui::SliderFloat("##Strength", &wind.Strength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Gust Strength", "Extra wind that comes and goes.");
            ImGui::SliderFloat("##Gust Strength", &wind.GustStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            PropertyLabel("Gust Frequency", "Gusts per second.");
            ImGui::SliderFloat("##Gust Frequency", &wind.GustFrequency, 0.0f, 2.0f, "%.2f",
                               ImGuiSliderFlags_AlwaysClamp);
            PropertyNote("Direction: the entity's forward axis (rotate it).");
        });

    ::UHE::DrawComponents<AnimatorComponent>(
        "Animator 3D", entity,
        [entity](AnimatorComponent& anim)
        {
            UHE::Entity ent = entity;
            if (!anim.Animator)
            {
                // Check if we have a model component
                if (ent.HasComponent<Model3DComponent>())
                {
                    auto& mc = ent.GetComponent<Model3DComponent>();
                    if (mc.IsLoaded && mc.ModelData)
                    {
                        PropertyLabel("Animator");
                        if (ImGui::Button("Initialize Animator", ImVec2(-FLT_MIN, 0.0f)))
                        {
                            anim.Animator = CreateRef<RD3d::Animator>(mc.ModelData);
                        }
                    }
                    else
                    {
                        PropertyNote("Model not loaded yet.");
                    }
                }
                else
                {
                    PropertyNote("Requires a Model3DComponent on the entity.");
                }
            }

            if (anim.Animator)
            {
                auto model = anim.Animator->GetModel();
                if (model && !model->GetAnimations().empty())
                {
                    const auto& animations = model->GetAnimations();

                    PropertyLabel("Animation");
                    if (ImGui::BeginCombo("##Animation", anim.CurrentAnimationName.empty()
                                                             ? "None"
                                                             : anim.CurrentAnimationName.c_str()))
                    {
                        for (const auto& a : animations)
                        {
                            bool isSelected = (anim.CurrentAnimationName == a.Name);
                            if (ImGui::Selectable(a.Name.c_str(), isSelected))
                            {
                                anim.CurrentAnimationName = a.Name;
                                anim.Animator->CrossFade(a.Name, anim.CrossFadeDuration);
                                anim.IsPlaying = true;
                            }
                            if (isSelected)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    PropertyLabel("Playback");
                    if (ImGui::Button(anim.IsPlaying ? "Pause" : "Play"))
                    {
                        anim.IsPlaying = !anim.IsPlaying;
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Restart"))
                    {
                        anim.Animator->PlayAnimation(anim.CurrentAnimationName);
                        anim.IsPlaying = true;
                    }

                    PropertyLabel("Playback Speed");
                    ImGui::DragFloat("##Playback Speed", &anim.PlaybackSpeed, 0.1f, 0.0f, 5.0f);

                    // Issue #41: loop mode, direction, blending and skin selection.
                    int loopMode = static_cast<int>(anim.LoopMode);
                    const char* loopNames[] = {"None", "Loop", "PingPong"};
                    PropertyLabel("Loop Mode");
                    if (ImGui::Combo("##Loop Mode", &loopMode, loopNames, 3))
                    {
                        anim.LoopMode = static_cast<RD3d::LoopMode>(loopMode);
                    }
                    PropertyLabel("Reverse");
                    ImGui::Checkbox("##Reverse", &anim.Reverse);
                    PropertyLabel("Root Motion");
                    ImGui::Checkbox("##Root Motion", &anim.RootMotion);
                    PropertyLabel("Cross Fade");
                    ImGui::DragFloat("##Cross Fade", &anim.CrossFadeDuration, 0.05f, 0.05f, 5.0f, "%.2fs");

                    if (model->GetSkinCount() > 0)
                    {
                        int skin = anim.SkinIndex;
                        PropertyLabel("Skin");
                        if (ImGui::SliderInt("##Skin", &skin, 0, static_cast<int>(model->GetSkinCount()) - 1))
                        {
                            anim.SkinIndex = skin;
                            anim.Animator->SetSkin(skin);
                        }
                    }

                    // Timeline scrubbing: drag through the current clip.
                    float duration = anim.Animator->GetDuration();
                    if (duration > 0.0f)
                    {
                        float t = anim.Animator->GetCurrentTime();
                        PropertyLabel("Timeline");
                        ImGui::SliderFloat("##Timeline", &t, 0.0f, duration, "%.2fs");
                        if (ImGui::IsItemEdited())
                        {
                            anim.Animator->Seek(t);
                        }
                    }
                }
                else
                {
                    PropertyNote("Model has no animations.");
                }
            }
        });

    ::UHE::DrawComponents<AudioSourceComponent>(
        "Audio Source", entity,
        [](AudioSourceComponent& audio)
        {
            PropertyLabel("Sound");
            ImGui::InputText("##Sound", &audio.SoundPath);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Project-relative path, e.g. Assets/Audio/engine.wav");

            PropertyLabel("Volume");
            ImGui::DragFloat("##Volume", &audio.Volume, 0.01f, 0.0f, 4.0f);
            PropertyLabel("Pitch");
            ImGui::DragFloat("##Pitch", &audio.Pitch, 0.01f, 0.05f, 4.0f);
            PropertyLabel("Loop");
            ImGui::Checkbox("##Loop", &audio.Loop);
            PropertyLabel("Mute");
            ImGui::Checkbox("##Mute", &audio.Muted);
            PropertyLabel("Autoplay", "Starts playing when Play mode begins.");
            ImGui::Checkbox("##Autoplay (Play mode)", &audio.Autoplay);

            PropertyLabel("Spatial 3D");
            ImGui::Checkbox("##Spatial 3D", &audio.Spatial);
            if (audio.Spatial)
            {
                PropertyLabel("Min Distance");
                ImGui::DragFloat("##Min Distance", &audio.MinDistance, 0.05f, 0.0f, 10000.0f);
                PropertyLabel("Max Distance");
                ImGui::DragFloat("##Max Distance", &audio.MaxDistance, 0.5f, 0.0f, 100000.0f);
                PropertyLabel("Rolloff");
                ImGui::DragFloat("##Rolloff", &audio.Rolloff, 0.01f, 0.0f, 4.0f);
                PropertyNote("Position follows the entity transform.");
            }
            else
            {
                PropertyLabel("Pan");
                ImGui::SliderFloat("##Pan", &audio.Pan, -1.0f, 1.0f);
            }

            PropertyLabel("Preview");
            if (ImGui::Button("Play"))
            {
                audio.PlayRequested = true;
                audio.RuntimeAutoStarted = true; // do not double-start in Play mode
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                audio.StopRequested = true;
            ImGui::SameLine();
            if (audio.RuntimePlaying)
                ImGui::TextColored(EditorTheme::Success(), "playing");
            else
                ImGui::TextDisabled("stopped");
        });
}

} // namespace UHE
