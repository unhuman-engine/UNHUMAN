#include "EditorDebugSettings.h"
#include <filesystem>
#include <fstream>
#include <string>
#include "UHE/Debug/GpuDebugOptions.h"
#include "UHE/Debug/RenderDoc.h"
#include "imgui.h"

namespace UHE::EditorDebugSettings
{
namespace
{
/// @brief Next to editor_theme.ini; static so its c_str() never outlives the string.
const std::string& SettingsPath()
{
    static const std::string path = (std::filesystem::current_path() / "editor_debug.ini").string();
    return path;
}
} // namespace

void Load()
{
    std::ifstream in(SettingsPath());
    if (!in)
        return; // first run: everything off
    GpuDebugOptions& options = GetGpuDebugOptions();
    std::string line;
    while (std::getline(in, line))
    {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const bool on = line.substr(eq + 1) == "1";
        if (key == "SyncValidation")
            options.RequestSyncValidation = on;
        else if (key == "RenderDoc")
            options.RequestRenderDoc = on;
    }
}

void Save()
{
    std::ofstream out(SettingsPath());
    if (!out)
        return;
    const GpuDebugOptions& options = GetGpuDebugOptions();
    out << "SyncValidation=" << (options.RequestSyncValidation ? 1 : 0) << '\n';
    out << "RenderDoc=" << (options.RequestRenderDoc ? 1 : 0) << '\n';
}

void Draw()
{
    GpuDebugOptions& options = GetGpuDebugOptions();
    bool changed = ImGui::Checkbox("Synchronization Validation", &options.RequestSyncValidation);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("The validation layer's hazard tracking: missing or wrong barriers between passes.\n"
                          "Slow; needs a build with validation layers. Same as -sync-validation.");
    changed |= ImGui::Checkbox("Load RenderDoc", &options.RequestRenderDoc);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("Frame captures from View > Capture Frame, F12 or gpu.capture. Same as -renderdoc.");
    if (changed)
        Save();
    ImGui::TextDisabled("Applies when the editor next starts.");
    if (options.RequestSyncValidation && options.RequestRenderDoc)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                           "RenderDoc mutes validation output while attached: use one at a time.");
    ImGui::TextDisabled("Now: validation %s, sync validation %s, RenderDoc %s", options.ValidationActive ? "on" : "off",
                        options.SyncValidationActive ? "on" : "off",
                        RenderDoc::Available() ? "attached" : "not loaded");
}

} // namespace UHE::EditorDebugSettings
