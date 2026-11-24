#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include "UHE/Audio/MiniAudioBackend.h"
#include "UHE/Core/Core.h"

namespace UHE::Audio
{
// Engine-facing audio facade. Backend handles stay opaque; the component and
// the editor's Audio panel drive voices through this surface.
class UHE_API AudioEngine
{
public:
    static void Init();
    static void Shutdown();
    static void Update();

    // ── playback ─────────────────────────────────────────────────────────
    static SoundHandle Play(const std::string& filepath, const SoundOptions& options);
    // Convenience overloads kept for existing one-shot call sites.
    static SoundHandle PlaySound2D(const std::string& filepath, float volume = 1.0f, bool loop = false);
    static SoundHandle PlaySound3D(const std::string& filepath, const glm::vec3& position, float volume = 1.0f,
                                   bool loop = false);

    static void Stop(SoundHandle handle);
    static void Pause(SoundHandle handle);
    static void Resume(SoundHandle handle);
    static void StopAll();
    static bool IsPlaying(SoundHandle handle);
    static bool IsValid(SoundHandle handle);

    // ── per-voice options ────────────────────────────────────────────────
    static void SetVolume(SoundHandle handle, float volume);
    static void SetPitch(SoundHandle handle, float pitch);
    static void SetPan(SoundHandle handle, float pan);
    static void SetPosition(SoundHandle handle, const glm::vec3& position);
    static void SetLooping(SoundHandle handle, bool loop);
    static void SetMinMaxDistance(SoundHandle handle, float minDistance, float maxDistance);
    static void SetRolloff(SoundHandle handle, float rolloff);

    // ── mixer / listener ─────────────────────────────────────────────────
    static void SetMasterVolume(float volume);
    static float GetMasterVolume();
    static void SetListenerPosition(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);

    static std::vector<ActiveVoice> GetActiveVoices();
};
} // namespace UHE::Audio
