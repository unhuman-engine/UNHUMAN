#include "Panel/AudioPanel.h"
#include <cstdio>
#include <string>
#include <vector>
#include "EditorTheme.h"
#include "UHE.h"
#include "UHE/Audio/AudioEngine.h"
#include "imgui.h"

namespace UHE
{
void AudioPanel::OnImGuiRender(bool& open)
{
    if (!ImGui::Begin("Audio", &open))
    {
        ImGui::End();
        return;
    }

    static bool masterInitialized = false;
    static float masterVolume = 1.0f;
    if (!masterInitialized)
    {
        masterVolume = Audio::AudioEngine::GetMasterVolume();
        masterInitialized = true;
    }

    std::vector<Audio::ActiveVoice> voices = Audio::AudioEngine::GetActiveVoices();

    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::SliderFloat("Master", &masterVolume, 0.0f, 2.0f, "%.2f"))
        Audio::AudioEngine::SetMasterVolume(masterVolume);
    ImGui::SameLine();
    ImGui::BeginDisabled(voices.empty());
    if (ImGui::Button("Stop All"))
    {
        Audio::AudioEngine::StopAll();
        voices.clear();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Play File..."))
    {
        const std::string file =
            FileDialogs::OpenFile("Audio (*.wav;*.ogg)\0*.wav;*.ogg\0", std::filesystem::current_path().string());
        if (!file.empty())
        {
            Audio::SoundOptions options;
            Audio::AudioEngine::Play(file, options);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu voice%s", voices.size(), voices.size() == 1 ? "" : "s");

    ImGui::Separator();

    if (voices.empty())
    {
        ImGui::TextDisabled("No active voices.");
        ImGui::TextDisabled("Add an Audio Source component or drop a .wav/.ogg into the viewport.");
        ImGui::End();
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##audioVoices", 5, flags))
    {
        ImGui::TableSetupColumn("Sound", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Pitch", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();

        for (Audio::ActiveVoice& voice : voices)
        {
            ImGui::PushID(static_cast<int>(voice.Handle));
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            const std::filesystem::path path(voice.Path);
            ImGui::TextUnformatted(path.filename().string().c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", voice.Path.c_str());

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            float volume = voice.Volume;
            if (ImGui::SliderFloat("##volume", &volume, 0.0f, 2.0f, "%.2f"))
                Audio::AudioEngine::SetVolume(voice.Handle, volume);

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1.0f);
            float pitch = voice.Pitch;
            if (ImGui::SliderFloat("##pitch", &pitch, 0.1f, 4.0f, "%.2f"))
                Audio::AudioEngine::SetPitch(voice.Handle, pitch);

            ImGui::TableSetColumnIndex(3);
            if (voice.Spatial)
            {
                ImGui::TextColored(EditorTheme::ConsoleInfo(), "3D");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Position: %.1f, %.1f, %.1f", voice.Position.x, voice.Position.y,
                                      voice.Position.z);
            }
            else
            {
                ImGui::TextDisabled("2D");
            }
            ImGui::SameLine();
            if (voice.Looping)
                ImGui::TextDisabled("loop");

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton(voice.Playing ? "Pause" : "Resume"))
            {
                if (voice.Playing)
                    Audio::AudioEngine::Pause(voice.Handle);
                else
                    Audio::AudioEngine::Resume(voice.Handle);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop"))
                Audio::AudioEngine::Stop(voice.Handle);

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::TextDisabled("The 3D listener follows the editor camera (and the primary camera in Play mode).");
    ImGui::End();
}
} // namespace UHE
