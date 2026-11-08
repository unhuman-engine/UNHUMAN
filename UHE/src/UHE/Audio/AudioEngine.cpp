#include "uhepch.h"
#include "AudioEngine.h"

namespace UHE::Audio
{
    void AudioEngine::Init()
    {
        MiniAudioBackend::Init();
    }

    void AudioEngine::Shutdown()
    {
        MiniAudioBackend::Shutdown();
    }

    void AudioEngine::Update()
    {
        MiniAudioBackend::Update();
    }

    SoundHandle AudioEngine::Play(const std::string& filepath, const SoundOptions& options)
    {
        return MiniAudioBackend::Play(filepath, options);
    }

    SoundHandle AudioEngine::PlaySound2D(const std::string& filepath, float volume, bool loop)
    {
        SoundOptions options;
        options.Volume = volume;
        options.Loop = loop;
        return MiniAudioBackend::Play(filepath, options);
    }

    SoundHandle AudioEngine::PlaySound3D(const std::string& filepath, const glm::vec3& position, float volume,
                                         bool loop)
    {
        SoundOptions options;
        options.Volume = volume;
        options.Loop = loop;
        options.Spatial = true;
        options.Position = position;
        return MiniAudioBackend::Play(filepath, options);
    }

    void AudioEngine::Stop(SoundHandle handle)
    {
        MiniAudioBackend::Stop(handle);
    }

    void AudioEngine::Pause(SoundHandle handle)
    {
        MiniAudioBackend::Pause(handle);
    }

    void AudioEngine::Resume(SoundHandle handle)
    {
        MiniAudioBackend::Resume(handle);
    }

    void AudioEngine::StopAll()
    {
        MiniAudioBackend::StopAll();
    }

    bool AudioEngine::IsPlaying(SoundHandle handle)
    {
        return MiniAudioBackend::IsPlaying(handle);
    }

    bool AudioEngine::IsValid(SoundHandle handle)
    {
        return MiniAudioBackend::IsValid(handle);
    }

    void AudioEngine::SetVolume(SoundHandle handle, float volume)
    {
        MiniAudioBackend::SetVolume(handle, volume);
    }

    void AudioEngine::SetPitch(SoundHandle handle, float pitch)
    {
        MiniAudioBackend::SetPitch(handle, pitch);
    }

    void AudioEngine::SetPan(SoundHandle handle, float pan)
    {
        MiniAudioBackend::SetPan(handle, pan);
    }

    void AudioEngine::SetPosition(SoundHandle handle, const glm::vec3& position)
    {
        MiniAudioBackend::SetPosition(handle, position);
    }

    void AudioEngine::SetLooping(SoundHandle handle, bool loop)
    {
        MiniAudioBackend::SetLooping(handle, loop);
    }

    void AudioEngine::SetMinMaxDistance(SoundHandle handle, float minDistance, float maxDistance)
    {
        MiniAudioBackend::SetMinMaxDistance(handle, minDistance, maxDistance);
    }

    void AudioEngine::SetRolloff(SoundHandle handle, float rolloff)
    {
        MiniAudioBackend::SetRolloff(handle, rolloff);
    }

    void AudioEngine::SetMasterVolume(float volume)
    {
        MiniAudioBackend::SetMasterVolume(volume);
    }

    float AudioEngine::GetMasterVolume()
    {
        return MiniAudioBackend::GetMasterVolume();
    }

    void AudioEngine::SetListenerPosition(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up)
    {
        MiniAudioBackend::SetListenerPosition(position, forward, up);
    }

    std::vector<ActiveVoice> AudioEngine::GetActiveVoices()
    {
        return MiniAudioBackend::GetActiveVoices();
    }
} // namespace UHE::Audio
