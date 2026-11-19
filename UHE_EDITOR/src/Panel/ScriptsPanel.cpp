#include "ScriptsPanel.h"

#include "EditorTheme.h"
#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Scene/Scene.h"
#include "UHE/Scripting/ScriptEngine.h"
#include "UHE/Utils/PlatfromUtils.h"

#include "imgui.h"

namespace UHE
{
void ScriptsPanel::OnImGuiRender(bool& open)
{
    UHE_PROFILE_FUNCTION();

    if (!ImGui::Begin("Scripts", &open))
    {
        ImGui::End();
        return;
    }

    if (m_Scene == nullptr)
    {
        ImGui::TextDisabled("No scene open.");
        ImGui::End();
        return;
    }

    ScriptEngine& engine = ScriptEngine::Get();
    if (!engine.IsInitialized())
    {
        ImGui::TextDisabled("Scripting is not initialized.");
        ImGui::End();
        return;
    }

    const std::vector<ScriptEngine::InstanceInfo> instances = engine.Instances(*m_Scene);

    ImGui::Checkbox("Show healthy", &m_ShowHealthy);
    ImGui::SameLine();
    if (ImGui::Button("Reload All"))
    {
        engine.ReloadAll(*m_Scene);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Recompiles every attached script and runs OnCreate again.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu attached", instances.size());

    ImGui::Separator();

    if (instances.empty())
    {
        ImGui::TextDisabled("No entity in this scene has a Script component.");
        ImGui::TextDisabled("Add one from the Properties panel, or right-click a .lua in the Content Browser.");
        ImGui::End();
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##scripts", 4, flags))
    {
        ImGui::TableSetupColumn("Entity", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Script", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        for (const ScriptEngine::InstanceInfo& info : instances)
        {
            if (!m_ShowHealthy && !info.Errored)
            {
                continue;
            }

            ImGui::PushID(static_cast<int>(info.EntityId));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(info.EntityName.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", info.Path.empty() ? "(no script)" : info.Path.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            const std::filesystem::path path(info.Path);
            ImGui::TextUnformatted(info.Path.empty() ? "-" : path.filename().string().c_str());

            ImGui::TableSetColumnIndex(2);
            if (info.Path.empty())
            {
                ImGui::TextDisabled("empty");
            }
            else if (info.Errored)
            {
                ImGui::TextColored(EditorTheme::ConsoleError(), "error");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", info.Error.c_str());
                }
            }
            else if (info.Started)
            {
                ImGui::TextColored(EditorTheme::Success(), "running");
            }
            else
            {
                ImGui::TextDisabled("waiting");
            }

            ImGui::TableSetColumnIndex(3);
            if (!info.Path.empty())
            {
                if (ImGui::SmallButton("Reload"))
                {
                    engine.ReloadEntity(*m_Scene, info.EntityId);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Open"))
                {
                    FileDialogs::OpenInDefaultApp(ResolveAssetPath(info.Path).string());
                }
            }

            // A failure is the interesting case, so it is shown in place rather
            // than only in a tooltip.
            if (info.Errored && !info.Error.empty())
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::ConsoleError());
                ImGui::TextWrapped("%s", info.Error.c_str());
                ImGui::PopStyleColor();
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
} // namespace UHE
