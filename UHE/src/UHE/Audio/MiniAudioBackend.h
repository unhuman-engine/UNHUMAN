#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace UHE::Audio
{
using SoundHandle = std::uint32_t;
inline constexpr SoundHandle kInvalidSound = 0;

// Options shared by one-shot playback and the AudioSource component.
struct SoundOptions
{
    float Volume = 1.0f;
    float Pitch = 1.0f;
    float Pan = 0.0f; // -1 left .. +1 right, ignored when Spatial
    bool Loop = false;
    bool Spatial = false;
    float MinDistance = 1.0f;
    float MaxDistance = 100.0f;
    float Rolloff = 1.0f;
    glm::vec3 Position{0.0f};
};

struct ActiveVoice
{
    SoundHandle Handle = kInvalidSound;
    std::string Path;
    float Volume = 1.0f;
    float Pitch = 1.0f;
    bool Spatial = false;
    bool Looping = false;
    bool Playing = false;
    glm::vec3 Position{0.0f};
};

class MiniAudioBackend
{
public:
    static void Init();
    static void Shutdown();
    static void Update();

    static SoundHandle Play(const std::string& filepath, const SoundOptions& options);
    static void Stop(SoundHandle handle);
    static void Pause(SoundHandle handle);
    static void Resume(SoundHandle handle);
    static void StopAll();
    static bool IsPlaying(SoundHandle handle);
    static bool IsValid(SoundHandle handle);

    static void SetVolume(SoundHandle handle, float volume);
    static void SetPitch(SoundHandle handle, float pitch);
    static void SetPan(SoundHandle handle, float pan);
    static void SetPosition(SoundHandle handle, const glm::vec3& position);
    static void SetLooping(SoundHandle handle, bool loop);
    static void SetMinMaxDistance(SoundHandle handle, float minDistance, float maxDistance);
    static void SetRolloff(SoundHandle handle, float rolloff);

    static void SetMasterVolume(float volume);
    static float GetMasterVolume();

    static void SetListenerPosition(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);

    static std::vector<ActiveVoice> GetActiveVoices();
};
} // namespace UHE::Audio
