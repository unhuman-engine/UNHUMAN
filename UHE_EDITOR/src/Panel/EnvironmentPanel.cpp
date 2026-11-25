// The Environment window: the open scene's RenderSettings, edited live.
#include "EnvironmentPanel.h"
#include <array>
#include <cfloat>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <string>
#include "SceneHierarchyStyle.h"
#include "UHE/Renderer3D/RenderSettings.h"
#include "UHE/Scene/Scene.h"

namespace UHE
{

namespace
{
/// @brief Window this panel docks beside the first time it opens without a saved layout.
constexpr const char* kDockSibling = "Properties";

/// @brief Slider flags shared by every ranged field: the range is the valid one, so Ctrl+click typing obeys it too.
constexpr ImGuiSliderFlags kClamp = ImGuiSliderFlags_AlwaysClamp;

/// @brief kClamp on a logarithmic scale, for a multiplier whose useful values bunch up near 1.
/// Combined as unsigned: ImGui's flag typedefs are int, and OR-ing signed ints is what lint rejects.
constexpr ImGuiSliderFlags kClampLogarithmic = static_cast<ImGuiSliderFlags>(
    static_cast<unsigned>(ImGuiSliderFlags_AlwaysClamp) | static_cast<unsigned>(ImGuiSliderFlags_Logarithmic));

/// @brief The bold UI font (Inter Bold), or null when only the default font is loaded.
ImFont* BoldFont()
{
    ImGuiIO& io = ImGui::GetIO();
    return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : nullptr;
}

// The property table and row below mirror the inspector's (SceneHierarchyInspector.cpp),
// so both panels share one label column ratio and read as the same kind of form.

/// @brief Opens a two-column label/value table; close it with EndPropertyTable() only when this returned true.
bool BeginPropertyTable(const char* id)
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

/// @brief Closes a table opened by BeginPropertyTable(); the cell padding is read by every row, so it pops here.
void EndPropertyTable()
{
    ImGui::EndTable();
    ImGui::PopStyleVar();
}

/**
 * @brief Starts a property row: the name in the left column, the next widget filling the right one.
 *
 * A name too long for its column ends in an ellipsis and shows in full as the tooltip.
 */
void PropertyLabel(const char* label, const char* tooltip = nullptr)
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

/// @brief Tree node flags of a section header: framed, open by default, and with room for the buttons drawn over it.
ImGuiTreeNodeFlags SectionHeaderFlags()
{
    unsigned flags = ImGuiTreeNodeFlags_DefaultOpen;
    flags |= ImGuiTreeNodeFlags_Framed;
    flags |= ImGuiTreeNodeFlags_SpanAvailWidth;
    flags |= ImGuiTreeNodeFlags_AllowOverlap;
    flags |= ImGuiTreeNodeFlags_FramePadding;
    flags |= ImGuiTreeNodeFlags_NoTreePushOnOpen;
    return static_cast<ImGuiTreeNodeFlags>(flags);
}

/// @brief What the user did in a section header this frame.
struct SectionState
{
    bool Open = false;  ///< the section's fields should be drawn
    bool Reset = false; ///< the Reset button was clicked
};

/**
 * @brief A collapsible section header in the inspector's style, with an Enabled checkbox and a Reset button.
 *
 * Both controls sit at the right end of the band, as in Godot's inspector,
 * so toggling a feature does not mean opening its section first.
 *
 * @param title   Visible section name; also its ID, which keeps the open state.
 * @param enabled The feature's switch, or null for a section that cannot be turned off.
 */
SectionState SectionHeader(const char* title, bool* enabled)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, style.FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImFont* boldFont = BoldFont();
    if (boldFont)
        ImGui::PushFont(boldFont, 0.0f);
    SectionState state;
    state.Open = ImGui::TreeNodeEx(title, SectionHeaderFlags());
    if (boldFont)
        ImGui::PopFont();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    const ImVec2 headerMin = ImGui::GetItemRectMin();
    const ImVec2 headerMax = ImGui::GetItemRectMax();
    const float height = headerMax.y - headerMin.y;
    const float resetWidth = ImGui::CalcTextSize("Reset").x + style.FramePadding.x * 2.0f;
    float x = headerMax.x - resetWidth;

    ImGui::PushID(title);
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(x, headerMin.y));
    // Flat on the band; the band is already the button colour.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    state.Reset = ImGui::Button("Reset", ImVec2(resetWidth, height));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Restore the defaults of %s", title);

    if (enabled)
    {
        x -= ImGui::GetFrameHeight() + style.ItemSpacing.x;
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(x, headerMin.y + (height - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::Checkbox("##Enabled", enabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip(*enabled ? "Enabled" : "Disabled");
    }
    ImGui::PopID();
    return state;
}

/// @brief A float slider row: the name on the left, the slider (clamped to its range) on the right.
bool SliderRow(const char* label, float& value, float min, float max, const char* format = "%.2f",
               const char* tooltip = nullptr, ImGuiSliderFlags flags = kClamp)
{
    PropertyLabel(label, tooltip);
    ImGui::PushID(label);
    const bool changed = ImGui::SliderFloat("##value", &value, min, max, format, flags);
    ImGui::PopID();
    return changed;
}

/// @brief A checkbox row.
bool CheckboxRow(const char* label, bool& value, const char* tooltip = nullptr)
{
    PropertyLabel(label, tooltip);
    ImGui::PushID(label);
    const bool changed = ImGui::Checkbox("##value", &value);
    ImGui::PopID();
    return changed;
}

/// @brief A colour row; colours are authored in sRGB, as the picker shows them.
bool ColorRow(const char* label, glm::vec3& value, const char* tooltip = nullptr)
{
    PropertyLabel(label, tooltip);
    ImGui::PushID(label);
    const bool changed = ImGui::ColorEdit3("##value", glm::value_ptr(value));
    ImGui::PopID();
    return changed;
}

/// @brief Opens a section's property table, greyed out when its feature is off.
/// @return Whether the table opened; pair with EndSection() only then.
bool BeginSection(const char* id, bool enabled)
{
    if (!BeginPropertyTable(id))
        return false;
    // Still visible, so the settings can be read before switching the feature on.
    ImGui::BeginDisabled(!enabled);
    return true;
}

void EndSection()
{
    ImGui::EndDisabled();
    EndPropertyTable();
    ImGui::Spacing();
}

void DrawEnvironmentSection(EnvironmentSettings& env)
{
    const SectionState section = SectionHeader("Environment", nullptr);
    if (section.Reset)
        env = EnvironmentSettings{};
    if (!section.Open || !BeginSection("##EnvironmentProperties", true))
        return;
    ColorRow("Sky Color", env.SkyColor, "Ambient light arriving from above.");
    ColorRow("Ground Color", env.GroundColor, "Ambient light bounced up from the ground.");
    SliderRow("Ambient Intensity", env.AmbientIntensity, 0.0f, 2.0f, "%.2f", "Multiplies both hemisphere colours.");
    SliderRow("Exposure", env.Exposure, 0.1f, 8.0f, "%.2f", "Linear scale applied before the tonemap curve.",
              kClampLogarithmic);
    EndSection();
}

void DrawShadowSection(ShadowSettings& shadows)
{
    const SectionState section = SectionHeader("Shadows", &shadows.Enabled);
    if (section.Reset)
        shadows = ShadowSettings{};
    if (!section.Open || !BeginSection("##ShadowProperties", shadows.Enabled))
        return;

    PropertyLabel("Cascades", "Each cascade covers a slice of the view distance.");
    ImGui::SliderInt("##Cascades", &shadows.CascadeCount, 1, 4, "%d", kClamp);
    SliderRow("Distance", shadows.Distance, 5.0f, 500.0f, "%.1f m",
              "View distance the last cascade reaches; nothing beyond it is shadowed.");
    SliderRow("Split Lambda", shadows.SplitLambda, 0.0f, 1.0f, "%.2f",
              "0 = uniform splits, 1 = logarithmic (more resolution near the camera).");

    static constexpr std::array<int, 4> kResolutions = {512, 1024, 2048, 4096};
    PropertyLabel("Resolution", "Texels per side of one cascade.");
    if (ImGui::BeginCombo("##Resolution", std::to_string(shadows.Resolution).c_str()))
    {
        for (const int resolution : kResolutions)
        {
            const bool selected = shadows.Resolution == resolution;
            if (ImGui::Selectable(std::to_string(resolution).c_str(), selected))
                shadows.Resolution = resolution;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    SliderRow("Bias", shadows.DepthBias, 0.0f, 5.0f, "%.2f", "Constant depth bias, in shadow-map texels.");
    SliderRow("Normal Bias", shadows.NormalBias, 0.0f, 5.0f, "%.2f",
              "Offsets the receiver along its normal, in shadow-map texels.");
    SliderRow("Softness", shadows.Softness, 0.0f, 4.0f, "%.2f", "Filter radius, in shadow-map texels.");
    CheckboxRow("Show Cascades", shadows.ShowCascades, "Tints each cascade (debug view).");
    EndSection();
}

void DrawSSAOSection(SSAOSettings& ssao)
{
    const SectionState section = SectionHeader("SSAO", &ssao.Enabled);
    if (section.Reset)
        ssao = SSAOSettings{};
    if (!section.Open || !BeginSection("##SSAOProperties", ssao.Enabled))
        return;
    SliderRow("Radius", ssao.Radius, 0.05f, 4.0f, "%.2f m", "World-space sample radius.");
    SliderRow("Intensity", ssao.Intensity, 0.0f, 4.0f, "%.2f", "Strength of the darkening.");
    SliderRow("Bias", ssao.Bias, 0.0f, 0.2f, "%.3f", "Depth bias against self-occlusion on flat surfaces.");
    EndSection();
}

void DrawBloomSection(BloomSettings& bloom)
{
    const SectionState section = SectionHeader("Bloom", &bloom.Enabled);
    if (section.Reset)
        bloom = BloomSettings{};
    if (!section.Open || !BeginSection("##BloomProperties", bloom.Enabled))
        return;
    SliderRow("Intensity", bloom.Intensity, 0.0f, 0.3f, "%.3f", "Share of the blurred image in the result.");
    SliderRow("Filter Radius", bloom.FilterRadius, 0.5f, 3.0f, "%.2f",
              "Upsample tent radius, in texels of the level being upsampled.");
    EndSection();
}

void DrawWindSection(WindSettings& wind)
{
    const SectionState section = SectionHeader("Wind", nullptr);
    if (section.Reset)
        wind = WindSettings{};
    if (!section.Open || !BeginSection("##WindProperties", true))
        return;

    // Stored as an (x, z) vector; edited as a compass angle, which is what a
    // person means by "wind direction". 0 degrees is +X, 90 is +Z.
    float degrees = 0.0f;
    if (glm::dot(wind.Direction, wind.Direction) > 1e-8f)
    {
        degrees = glm::degrees(std::atan2(wind.Direction.y, wind.Direction.x));
        if (degrees < 0.0f)
            degrees += 360.0f;
    }
    PropertyLabel("Direction", "Angle on the ground plane: 0 = +X, 90 = +Z.");
    if (ImGui::SliderFloat("##Direction", &degrees, 0.0f, 360.0f, "%.0f\xC2\xB0", kClamp))
    {
        const float radians = glm::radians(degrees);
        wind.Direction = glm::vec2(std::cos(radians), std::sin(radians));
    }
    SliderRow("Strength", wind.Strength, 0.0f, 2.0f, "%.2f", "Steady wind.");
    SliderRow("Gust Strength", wind.GustStrength, 0.0f, 2.0f, "%.2f", "Extra wind that comes and goes.");
    SliderRow("Gust Frequency", wind.GustFrequency, 0.0f, 2.0f, "%.2f /s", "Gusts per second.");
    EndSection();
}
} // namespace

void EnvironmentPanel::OnImGuiRender(bool& open)
{
    UHE_PROFILE_FUNCTION();

    // A layout saved before this panel existed has no place for it: join the
    // inspector's dock node the first time instead of floating over the viewport.
    // FindWindowByName and DockId are read through exported functions and fields,
    // not GImGui, so this links against the engine DLL on Windows.
    if (const ImGuiWindow* sibling = ImGui::FindWindowByName(kDockSibling); sibling && sibling->DockId != 0)
        ImGui::SetNextWindowDockID(sibling->DockId, ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Environment", &open))
    {
        ImGui::End();
        return;
    }

    if (!m_Scene)
    {
        ImGui::TextDisabled("No scene open.");
        ImGui::End();
        return;
    }

    RenderSettings& settings = m_Scene->GetRenderSettings();
    DrawEnvironmentSection(settings.Environment);
    DrawShadowSection(settings.Shadows);
    DrawSSAOSection(settings.SSAO);
    DrawBloomSection(settings.Bloom);
    DrawWindSection(settings.Wind);

    ImGui::End();
}

} // namespace UHE
