#include "EditorUI.h"

#include "EditorTheme.h"

#include <cmath>
#include <cstdio>

namespace EditorUI
{
    namespace
    {
        float Luminance(const ImVec4& c)
        {
            return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
        }

        ImU32 ContrastText(const ImVec4& background)
        {
            return Luminance(background) > 0.55f ? IM_COL32(20, 20, 24, 255) : IM_COL32(245, 245, 245, 255);
        }

        void DrawArrow(ImDrawList* draw, const ImVec2& from, const ImVec2& dir, float length, float head, ImU32 color)
        {
            const ImVec2 tip(from.x + dir.x * length, from.y + dir.y * length);
            const ImVec2 normal(-dir.y, dir.x);
            draw->AddLine(from, tip, color, 2.0f);
            draw->AddTriangleFilled(
                tip,
                ImVec2(tip.x - dir.x * head + normal.x * head * 0.55f, tip.y - dir.y * head + normal.y * head * 0.55f),
                ImVec2(tip.x - dir.x * head - normal.x * head * 0.55f, tip.y - dir.y * head - normal.y * head * 0.55f),
                color);
        }

        void DrawToolIcon(ImDrawList* draw, const ImVec2& center, float size, ToolIcon icon, ImU32 color)
        {
            const float unit = size;
            switch (icon)
            {
                case ToolIcon::Select:
                {
                    const ImVec2 p[7] = {
                        ImVec2(center.x - 0.18f * unit, center.y - 0.34f * unit),
                        ImVec2(center.x - 0.18f * unit, center.y + 0.32f * unit),
                        ImVec2(center.x - 0.02f * unit, center.y + 0.16f * unit),
                        ImVec2(center.x + 0.08f * unit, center.y + 0.34f * unit),
                        ImVec2(center.x + 0.22f * unit, center.y + 0.27f * unit),
                        ImVec2(center.x + 0.12f * unit, center.y + 0.09f * unit),
                        ImVec2(center.x + 0.30f * unit, center.y + 0.05f * unit),
                    };
                    draw->AddPolyline(p, 7, color, ImDrawFlags_Closed, 1.8f);
                    draw->AddTriangleFilled(p[0], p[1], p[2], color);
                    break;
                }
                case ToolIcon::Move:
                    DrawArrow(draw, center, ImVec2(1, 0), 0.30f * unit, 0.10f * unit, color);
                    DrawArrow(draw, center, ImVec2(-1, 0), 0.30f * unit, 0.10f * unit, color);
                    DrawArrow(draw, center, ImVec2(0, 1), 0.30f * unit, 0.10f * unit, color);
                    DrawArrow(draw, center, ImVec2(0, -1), 0.30f * unit, 0.10f * unit, color);
                    break;
                case ToolIcon::Rotate:
                {
                    const float radius = 0.24f * unit;
                    draw->PathArcTo(center, radius, -2.55f, 1.75f, 28);
                    draw->PathStroke(color, 0, 2.0f);
                    const float angle = 1.75f;
                    const ImVec2 tip(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
                    const ImVec2 tangent(-std::sin(angle), std::cos(angle));
                    const ImVec2 normal(std::cos(angle), std::sin(angle));
                    draw->AddTriangleFilled(
                        tip,
                        ImVec2(tip.x - tangent.x * 0.13f * unit + normal.x * 0.06f * unit,
                               tip.y - tangent.y * 0.13f * unit + normal.y * 0.06f * unit),
                        ImVec2(tip.x - tangent.x * 0.13f * unit - normal.x * 0.06f * unit,
                               tip.y - tangent.y * 0.13f * unit - normal.y * 0.06f * unit),
                        color);
                    break;
                }
                case ToolIcon::Scale:
                {
                    const ImVec2 squareMin(center.x - 0.28f * unit, center.y - 0.28f * unit);
                    const ImVec2 squareMax(center.x + 0.00f * unit, center.y + 0.00f * unit);
                    draw->AddRect(squareMin, squareMax, color, 2.0f, 0, 1.6f);
                    DrawArrow(draw, squareMax, ImVec2(0.7071f, 0.7071f), 0.28f * unit, 0.10f * unit, color);
                    break;
                }
            }
        }
        /// A sphere for each shading mode, in \p color (the button's foreground).
        void DrawShadingIcon(ImDrawList* draw, const ImVec2& c, float r, ShadingIcon icon, ImU32 color)
        {
            const ImVec4 base = ImGui::ColorConvertU32ToFloat4(color);
            auto tone = [&](float k, float a = 1.0f)
            { return ImGui::GetColorU32(ImVec4(base.x * k, base.y * k, base.z * k, base.w * a)); };
            switch (icon)
            {
                case ShadingIcon::Wireframe:
                    draw->AddCircle(c, r, color, 24, 1.3f);
                    draw->AddEllipse(c, ImVec2(r, r * 0.38f), color, 0.0f, 24, 1.0f);
                    draw->AddEllipse(c, ImVec2(r * 0.38f, r), color, 0.0f, 24, 1.0f);
                    break;
                case ShadingIcon::Solid:
                    // Clay: a lit disc with its shadow side darker.
                    draw->AddCircleFilled(c, r, tone(0.55f), 24);
                    draw->AddCircleFilled(ImVec2(c.x - r * 0.18f, c.y - r * 0.18f), r * 0.78f, tone(0.85f), 24);
                    break;
                case ShadingIcon::Unlit:
                    draw->AddCircleFilled(c, r, color, 24); // flat: no shading at all
                    break;
                case ShadingIcon::Lit:
                    // Material: dark base, a soft lit side and a specular dot.
                    draw->AddCircleFilled(c, r, tone(0.40f), 24);
                    draw->AddCircleFilled(ImVec2(c.x - r * 0.15f, c.y - r * 0.15f), r * 0.80f, tone(0.70f), 24);
                    draw->AddCircleFilled(ImVec2(c.x - r * 0.28f, c.y - r * 0.28f), r * 0.50f, tone(0.95f), 24);
                    draw->AddCircleFilled(ImVec2(c.x - r * 0.38f, c.y - r * 0.40f), r * 0.16f,
                                          IM_COL32(255, 255, 255, 230), 12);
                    break;
                case ShadingIcon::Normals:
                {
                    // Three wedges in the colours a normal view paints x, y and z.
                    const ImU32 wedges[3] = {IM_COL32(222, 96, 104, 255), IM_COL32(120, 210, 110, 255),
                                             IM_COL32(100, 150, 240, 255)};
                    for (int i = 0; i < 3; ++i)
                    {
                        const float a0 = -1.5708f + i * 2.0944f;
                        draw->PathLineTo(c);
                        draw->PathArcTo(c, r, a0, a0 + 2.0944f, 10);
                        draw->PathFillConvex(wedges[i]);
                    }
                    draw->AddCircle(c, r, tone(1.0f, 0.6f), 24, 1.0f);
                    break;
                }
            }
        }
    } // namespace

    bool ShadingButton(const char* id, ShadingIcon icon, bool active, const char* tooltip, float size)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 max(pos.x + size, pos.y + size);
        if (active)
            draw->AddRectFilled(pos, max, ImGui::GetColorU32(EditorTheme::Accent()), 4.0f);
        else if (held || hovered)
            draw->AddRectFilled(pos, max, ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered),
                                4.0f);

        const ImU32 foreground = active ? ImGui::GetColorU32(EditorTheme::AccentForeground())
                                        : ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        DrawShadingIcon(draw, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.32f, icon, foreground);

        if (tooltip && hovered)
            ImGui::SetTooltip("%s", tooltip);
        return ImGui::IsItemClicked();
    }

    bool HeaderMenuButton(const char* label, const char* popupId, const char* tooltip)
    {
        const bool open = ImGui::IsPopupOpen(popupId);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              open ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0));
        // Trailing spaces reserve room for a drawn chevron: the font has no arrow glyph.
        char text[96];
        std::snprintf(text, sizeof(text), "%s     ", label);
        const bool clicked = ImGui::Button(text);
        ImGui::PopStyleColor();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float arrowX = max.x - ImGui::GetStyle().FramePadding.x - 5.0f;
        const float arrowY = (ImGui::GetItemRectMin().y + max.y) * 0.5f;
        ImGui::GetWindowDrawList()->AddTriangleFilled(
            ImVec2(arrowX - 4.0f, arrowY - 2.0f), ImVec2(arrowX + 4.0f, arrowY - 2.0f), ImVec2(arrowX, arrowY + 3.0f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled));
        if (tooltip && ImGui::IsItemHovered() && !open)
            ImGui::SetTooltip("%s", tooltip);
        if (clicked)
            ImGui::OpenPopup(popupId);
        return ImGui::IsPopupOpen(popupId);
    }

    bool ToolIconButton(const char* id, ToolIcon icon, bool active, const char* tooltip, float size)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 max(pos.x + size, pos.y + size);

        ImU32 background = 0;
        if (active)
            background = ImGui::GetColorU32(EditorTheme::Accent());
        else if (held)
            background = ImGui::GetColorU32(ImGuiCol_ButtonActive);
        else if (hovered)
            background = ImGui::GetColorU32(ImGuiCol_ButtonHovered);

        if (background)
            draw->AddRectFilled(pos, max, background, 5.0f);

        const ImU32 foreground = active
            ? ImGui::GetColorU32(EditorTheme::AccentForeground())
            : ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);

        draw->PushClipRect(pos, max, true);
        DrawToolIcon(draw, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), size, icon, foreground);
        draw->PopClipRect();

        if (tooltip && hovered)
            ImGui::SetTooltip("%s", tooltip);
        return ImGui::IsItemClicked();
    }

    bool TransportButton(const char* id, TransportIcon icon, bool enabled, bool active,
                         const char* tooltip, const ImVec4& color, float size)
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 max(pos.x + size, pos.y + size);

        if (hovered || active)
        {
            const ImVec4 background = active
                ? ImVec4(color.x, color.y, color.z, 0.30f)
                : ImGui::GetStyleColorVec4(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
            draw->AddRectFilled(pos, max, ImGui::GetColorU32(background), 6.0f);
        }

        const ImU32 foreground = enabled
            ? ImGui::GetColorU32(color)
            : ImGui::GetColorU32(ImGuiCol_TextDisabled);

        const ImVec2 center(pos.x + size * 0.5f, pos.y + size * 0.5f);
        const float unit = size;

        draw->PushClipRect(pos, max, true);
        switch (icon)
        {
            case TransportIcon::Play:
                draw->AddTriangleFilled(
                    ImVec2(center.x - 0.15f * unit, center.y - 0.28f * unit),
                    ImVec2(center.x - 0.15f * unit, center.y + 0.28f * unit),
                    ImVec2(center.x + 0.29f * unit, center.y),
                    foreground);
                break;
            case TransportIcon::Pause:
                draw->AddRectFilled(
                    ImVec2(center.x - 0.18f * unit, center.y - 0.28f * unit),
                    ImVec2(center.x - 0.04f * unit, center.y + 0.28f * unit),
                    foreground, 1.0f);
                draw->AddRectFilled(
                    ImVec2(center.x + 0.04f * unit, center.y - 0.28f * unit),
                    ImVec2(center.x + 0.18f * unit, center.y + 0.28f * unit),
                    foreground, 1.0f);
                break;
            case TransportIcon::Stop:
                draw->AddRectFilled(
                    ImVec2(center.x - 0.22f * unit, center.y - 0.22f * unit),
                    ImVec2(center.x + 0.22f * unit, center.y + 0.22f * unit),
                    foreground, 2.0f);
                break;
        }
        draw->PopClipRect();

        if (tooltip && hovered)
            ImGui::SetTooltip("%s", tooltip);
        return enabled && ImGui::IsItemClicked();
    }

    bool ToolbarToggle(const char* label, bool active, const char* tooltip, const ImVec2& size)
    {
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Accent());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::AccentHover());
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::AccentActive());
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::AccentForeground());
        }

        const bool clicked = ImGui::Button(label, size);

        if (active)
            ImGui::PopStyleColor(4);

        if (tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        return clicked;
    }

    void SectionHeader(const char* label)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Accent());
        ImGui::SeparatorText(label);
        ImGui::PopStyleColor();
    }

    void Badge(const char* label, const ImVec4& color)
    {
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        const ImVec2 padding(8.0f, 3.0f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(color), size.y * 0.5f);
        draw->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y), ContrastText(color), label);
        ImGui::Dummy(size);
    }

    bool SearchField(const char* id, char* buffer, std::size_t bufferSize, const char* hint, float width)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        if (width > 0.0f)
            ImGui::SetNextItemWidth(width);
        const bool changed = ImGui::InputTextWithHint(id, hint, buffer, bufferSize);
        ImGui::PopStyleVar();
        return changed;
    }

    bool ToggleMenuItem(const char* label, bool* value, const char* shortcut)
    {
        // ImGui::MenuItem already flips *value when p_selected is passed, so the
        // previous manual flip cancelled it out and the toggles did nothing.
        return ImGui::MenuItem(label, shortcut, value);
    }

    void Separator()
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetTextLineHeight();
        ImGui::Dummy(ImVec2(1.0f, height));
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(pos.x, pos.y + 2.0f), ImVec2(pos.x, pos.y + height - 2.0f),
            ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
    }
} // namespace EditorUI
