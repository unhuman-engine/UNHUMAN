#include "EditorDisplaySettings.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>
#include "EditorUI.h"
#include "imgui.h"

namespace UHE
{
namespace
{
struct SizePreset
{
    u32 Width;
    u32 Height;
};
constexpr SizePreset kWindowSizePresets[] = {
    {.Width = 1280, .Height = 720},
    {.Width = 1600, .Height = 900},
    {.Width = 1920, .Height = 1080},
    {.Width = 2560, .Height = 1440},
};
constexpr int kRenderScalePresets[] = {50, 75, 100, 150, 200};
constexpr const char* kModeNames[] = {"Windowed", "Borderless", "Fullscreen"};

/// @brief Next to editor_theme.ini. Static for the same reason as the theme's:
/// a temporary path's c_str() outlived its string there.
const std::string& SettingsPath()
{
    static const std::string path = (std::filesystem::current_path() / "editor_display.ini").string();
    return path;
}

/// @brief Opens the label/value table the Display rows share, label column on the left like the inspector's.
bool BeginRows(const char* id)
{
    unsigned flags = ImGuiTableFlags_SizingStretchProp;
    flags |= ImGuiTableFlags_NoSavedSettings;
    if (!ImGui::BeginTable(id, 2, static_cast<ImGuiTableFlags>(flags)))
        return false;
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
    return true;
}

/// @brief Starts a row: \p label on the left, the next item filling the value column.
void RowLabel(const char* label, const char* tooltip = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", tooltip);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

std::string ModeLabel(const VideoMode& mode)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%u x %u @ %u Hz", mode.Width, mode.Height, mode.RefreshRate);
    return text;
}
} // namespace

void EditorDisplaySettings::Load()
{
    std::ifstream in(SettingsPath());
    if (!in)
        return; // first run: defaults

    std::string line;
    while (std::getline(in, line))
    {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        std::istringstream values(value);

        if (key == "mode")
        {
            int mode = 0;
            if (values >> mode && mode >= 0 && mode <= static_cast<int>(WindowMode::Fullscreen))
                m_Mode = static_cast<WindowMode>(mode);
        }
        else if (key == "monitor")
        {
            m_MonitorName = value;
        }
        else if (key == "monitor_index")
        {
            values >> m_MonitorIndex;
        }
        else if (key == "windowed")
        {
            u32 width = 0, height = 0;
            if (values >> width >> height)
            {
                m_WindowedWidth = width;
                m_WindowedHeight = height;
            }
        }
        else if (key == "fullscreen")
        {
            u32 width = 0, height = 0, refresh = 0;
            if (values >> width >> height >> refresh)
            {
                m_FullscreenWidth = width;
                m_FullscreenHeight = height;
                m_FullscreenRefresh = refresh;
            }
        }
        else if (key == "vsync")
        {
            int vsync = 0;
            if (values >> vsync)
                m_VSync = vsync != 0;
        }
        else if (key == "render_scale")
        {
            f32 scale = 1.0f;
            if (values >> scale && std::isfinite(scale))
                m_RenderScale = std::clamp(scale, kMinRenderScale, kMaxRenderScale);
        }
    }
}

void EditorDisplaySettings::Save() const
{
    std::ofstream out(SettingsPath());
    if (!out)
        return; // a read-only directory must not break the UI
    out << "mode=" << static_cast<int>(m_Mode) << '\n'
        << "monitor=" << m_MonitorName << '\n'
        << "monitor_index=" << m_MonitorIndex << '\n'
        << "windowed=" << m_WindowedWidth << ' ' << m_WindowedHeight << '\n'
        << "fullscreen=" << m_FullscreenWidth << ' ' << m_FullscreenHeight << ' ' << m_FullscreenRefresh << '\n'
        << "vsync=" << (m_VSync ? 1 : 0) << '\n'
        << "render_scale=" << m_RenderScale << '\n';
}

void EditorDisplaySettings::ApplyAll(Window& window)
{
    window.SetVSync(m_VSync);
    m_ModeDirty = m_Mode != WindowMode::Windowed;
    m_MonitorDirty = !m_MonitorName.empty();
    m_SizeDirty = true;
    ApplyPending(window);
}

void EditorDisplaySettings::ApplyPending(Window& window)
{
    if (m_ModeDirty || m_MonitorDirty)
    {
        const i32 monitor = ResolveMonitor(window);
        if (m_Mode != WindowMode::Windowed)
            window.SetWindowMode(m_Mode, monitor, m_FullscreenWidth, m_FullscreenHeight, m_FullscreenRefresh);
        else if (window.GetWindowMode() != WindowMode::Windowed)
            window.SetWindowMode(WindowMode::Windowed);
        // The other modes take the monitor as a parameter; a window changes monitor by moving.
        if (m_Mode == WindowMode::Windowed && m_MonitorDirty && monitor >= 0 && monitor != window.GetCurrentMonitor())
            window.MoveToMonitor(monitor);
    }
    // After the mode: leaving Windowed records the window's size at that
    // moment, and the chosen size has to win over it.
    if (m_SizeDirty && m_WindowedWidth > 0 && m_WindowedHeight > 0)
        window.SetWindowedSize(m_WindowedWidth, m_WindowedHeight);

    m_ModeDirty = false;
    m_MonitorDirty = false;
    m_SizeDirty = false;
}

void EditorDisplaySettings::CaptureWindowSize(const Window& window)
{
    if (window.GetWindowMode() != WindowMode::Windowed || window.GetWidth() == 0 || window.GetHeight() == 0)
        return;
    m_WindowedWidth = window.GetWidth();
    m_WindowedHeight = window.GetHeight();
}

u32 EditorDisplaySettings::ScaledSize(f32 panelSize, u32 maxSize) const
{
    const f32 scaled = std::round(panelSize * m_RenderScale);
    return static_cast<u32>(std::clamp(scaled, 1.0f, static_cast<f32>(std::max(maxSize, 1u))));
}

i32 EditorDisplaySettings::ResolveMonitor(const Window& window) const
{
    if (m_MonitorName.empty())
        return -1;
    const std::vector<MonitorInfo> monitors = window.GetMonitors();
    const i32 count = static_cast<i32>(monitors.size());
    if (m_MonitorIndex >= 0 && m_MonitorIndex < count && monitors[m_MonitorIndex].Name == m_MonitorName)
        return m_MonitorIndex;
    // Monitors are renumbered when one is plugged in or out; the name survives that.
    for (i32 i = 0; i < count; ++i)
    {
        if (monitors[i].Name == m_MonitorName)
            return i;
    }
    return -1;
}

void EditorDisplaySettings::Draw(Window& window)
{
    const std::vector<MonitorInfo> monitors = window.GetMonitors();
    i32 monitor = ResolveMonitor(window);
    if (monitor < 0)
        monitor = window.GetCurrentMonitor();
    const MonitorInfo* monitorInfo =
        monitor >= 0 && std::cmp_less(monitor, monitors.size()) ? &monitors[monitor] : nullptr;

    if (!BeginRows("##DisplaySettings"))
        return;

    RowLabel("Window Mode", "Borderless covers the monitor at its desktop resolution; Fullscreen is exclusive.");
    int mode = static_cast<int>(m_Mode);
    if (ImGui::Combo("##WindowMode", &mode, kModeNames, IM_ARRAYSIZE(kModeNames)) && mode != static_cast<int>(m_Mode))
    {
        m_Mode = static_cast<WindowMode>(mode);
        m_ModeDirty = true;
        Save();
    }

    if (monitors.size() > 1)
    {
        RowLabel("Monitor");
        char preview[160] = "";
        if (monitorInfo != nullptr)
            std::snprintf(preview, sizeof(preview), "%d: %s", monitor + 1, monitorInfo->Name.c_str());
        if (ImGui::BeginCombo("##Monitor", preview))
        {
            for (i32 i = 0; std::cmp_less(i, monitors.size()); ++i)
            {
                char label[192];
                std::snprintf(label, sizeof(label), "%d: %s (%u x %u)##monitor%d", i + 1, monitors[i].Name.c_str(),
                              monitors[i].Width, monitors[i].Height, i);
                const bool selected = i == monitor;
                if (ImGui::Selectable(label, selected) && !selected)
                {
                    m_MonitorName = monitors[i].Name;
                    m_MonitorIndex = i;
                    // A resolution belongs to the monitor it was picked on.
                    m_FullscreenWidth = m_FullscreenHeight = m_FullscreenRefresh = 0;
                    m_MonitorDirty = true;
                    Save();
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    if (m_Mode == WindowMode::Windowed)
    {
        RowLabel("Window Size", "Presets larger than the monitor are unavailable.");
        const u32 width = window.GetWidth();
        const u32 height = window.GetHeight();
        char preview[64];
        std::snprintf(preview, sizeof(preview), "%u x %u", width, height);
        if (ImGui::BeginCombo("##WindowSize", preview))
        {
            const bool isPreset = std::ranges::any_of(kWindowSizePresets, [&](const SizePreset& preset)
                                                      { return preset.Width == width && preset.Height == height; });
            if (!isPreset)
            {
                char current[64];
                std::snprintf(current, sizeof(current), "%u x %u (current)", width, height);
                ImGui::Selectable(current, true);
            }
            for (const SizePreset& preset : kWindowSizePresets)
            {
                char label[32];
                std::snprintf(label, sizeof(label), "%u x %u", preset.Width, preset.Height);
                const bool fits = monitorInfo == nullptr ||
                                  (preset.Width <= monitorInfo->Width && preset.Height <= monitorInfo->Height);
                const bool selected = preset.Width == width && preset.Height == height;
                ImGui::BeginDisabled(!fits);
                if (ImGui::Selectable(label, selected) && !selected)
                {
                    m_WindowedWidth = preset.Width;
                    m_WindowedHeight = preset.Height;
                    m_SizeDirty = true;
                    Save();
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
    }
    else if (m_Mode == WindowMode::Fullscreen)
    {
        RowLabel("Resolution", "Desktop keeps the monitor's current video mode, so switching to it is instant.");
        std::vector<VideoMode> modes = window.GetVideoModes(monitor);
        // Largest first, one entry per distinct mode.
        std::ranges::sort(modes,
                          [](const VideoMode& a, const VideoMode& b)
                          {
                              if (a.Width != b.Width)
                                  return a.Width > b.Width;
                              if (a.Height != b.Height)
                                  return a.Height > b.Height;
                              return a.RefreshRate > b.RefreshRate;
                          });
        const auto duplicates = std::ranges::unique(
            modes, [](const VideoMode& a, const VideoMode& b)
            { return a.Width == b.Width && a.Height == b.Height && a.RefreshRate == b.RefreshRate; });
        modes.erase(duplicates.begin(), duplicates.end());

        const bool desktop = m_FullscreenWidth == 0 || m_FullscreenHeight == 0;
        const std::string preview =
            desktop
                ? std::string("Desktop")
                : ModeLabel(
                      {.Width = m_FullscreenWidth, .Height = m_FullscreenHeight, .RefreshRate = m_FullscreenRefresh});
        if (ImGui::BeginCombo("##FullscreenResolution", preview.c_str()))
        {
            std::string desktopLabel = "Desktop";
            if (monitorInfo != nullptr)
                desktopLabel += " (" +
                                ModeLabel({.Width = monitorInfo->Width,
                                           .Height = monitorInfo->Height,
                                           .RefreshRate = monitorInfo->RefreshRate}) +
                                ")";
            if (ImGui::Selectable(desktopLabel.c_str(), desktop) && !desktop)
            {
                m_FullscreenWidth = m_FullscreenHeight = m_FullscreenRefresh = 0;
                m_ModeDirty = true;
                Save();
            }
            for (const VideoMode& videoMode : modes)
            {
                const bool selected = !desktop && videoMode.Width == m_FullscreenWidth &&
                                      videoMode.Height == m_FullscreenHeight &&
                                      videoMode.RefreshRate == m_FullscreenRefresh;
                if (ImGui::Selectable(ModeLabel(videoMode).c_str(), selected) && !selected)
                {
                    m_FullscreenWidth = videoMode.Width;
                    m_FullscreenHeight = videoMode.Height;
                    m_FullscreenRefresh = videoMode.RefreshRate;
                    m_ModeDirty = true;
                    Save();
                }
            }
            ImGui::EndCombo();
        }
    }

    RowLabel("VSync", "On presents in step with the display (FIFO). Off presents as soon as a frame is ready "
                      "(MAILBOX, or IMMEDIATE where the GPU has no mailbox mode).");
    if (ImGui::Checkbox("##VSync", &m_VSync))
    {
        window.SetVSync(m_VSync);
        Save();
    }

    RowLabel("Render Scale", "Scene viewport resolution relative to the panel. The image is still shown at "
                             "panel size: lower is faster, higher is sharper.");
    int percent = static_cast<int>(std::lround(m_RenderScale * 100.0f));
    if (ImGui::SliderInt("##RenderScale", &percent, static_cast<int>(kMinRenderScale * 100.0f),
                         static_cast<int>(kMaxRenderScale * 100.0f), "%d%%", ImGuiSliderFlags_AlwaysClamp))
        m_RenderScale = std::clamp(static_cast<f32>(percent) / 100.0f, kMinRenderScale, kMaxRenderScale);
    // Written once the drag ends, not on every frame of it.
    if (ImGui::IsItemDeactivatedAfterEdit())
        Save();

    const ImGuiStyle& style = ImGui::GetStyle();
    const int presetCount = IM_ARRAYSIZE(kRenderScalePresets);
    const float buttonWidth =
        std::max((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * static_cast<float>(presetCount - 1)) /
                     static_cast<float>(presetCount),
                 1.0f);
    for (int i = 0; i < presetCount; ++i)
    {
        if (i > 0)
            ImGui::SameLine();
        char label[16];
        std::snprintf(label, sizeof(label), "%d%%", kRenderScalePresets[i]);
        const bool active = percent == kRenderScalePresets[i];
        if (EditorUI::ToolbarToggle(label, active, nullptr, ImVec2(buttonWidth, 0.0f)) && !active)
        {
            m_RenderScale = static_cast<f32>(kRenderScalePresets[i]) / 100.0f;
            Save();
        }
    }

    ImGui::EndTable();
}
} // namespace UHE
