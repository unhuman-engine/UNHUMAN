#include "uhepch.h"
#include "MiniAudioBackend.h"

#include "UHE/AssetsManager/VfsSystem.h"
#include "UHE/Core/Log.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_FLAC // Bypass dr_flac GCC -O0 SSE4.1 compiler bug
#define MA_NO_MP3
#include <miniaudio.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace UHE::Audio
{
    namespace
    {
        struct Voice
        {
            ma_sound* Sound = nullptr;
            ActiveVoice Info;
        };

        ma_engine s_AudioEngine;
        bool s_Initialized = false;
        float s_MasterVolume = 1.0f;
        SoundHandle s_NextHandle = 1;
        std::unordered_map<SoundHandle, Voice> s_Voices;

        Voice* FindVoice(SoundHandle handle)
        {
            auto it = s_Voices.find(handle);
            return it != s_Voices.end() ? &it->second : nullptr;
        }

        void DestroyVoice(SoundHandle handle)
        {
            auto it = s_Voices.find(handle);
            if (it == s_Voices.end())
                return;

            ma_sound_stop(it->second.Sound);
            ma_sound_uninit(it->second.Sound);
            delete it->second.Sound;
            s_Voices.erase(it);
        }
    } // namespace

    void MiniAudioBackend::Init()
    {
        ma_engine_config engineConfig = ma_engine_config_init();
        engineConfig.listenerCount = 1;

        ma_result result = ma_engine_init(&engineConfig, &s_AudioEngine);
        if (result != MA_SUCCESS)
        {
            UHE_ERROR("Failed to initialize MiniAudio engine. Error code: {}", (int)result);
            return;
        }

        s_Initialized = true;
        ma_engine_set_volume(&s_AudioEngine, s_MasterVolume);
        UHE_INFO("MiniAudio Backend Initialized Successfully");
    }

    void MiniAudioBackend::Shutdown()
    {
        if (!s_Initialized)
            return;

        StopAll();
        ma_engine_uninit(&s_AudioEngine);
        s_Initialized = false;
        UHE_INFO("MiniAudio Backend Shut Down");
    }

    void MiniAudioBackend::Update()
    {
        if (!s_Initialized)
            return;

        // Finished sounds are gone; paused voices stay so they can resume.
        for (auto it = s_Voices.begin(); it != s_Voices.end();)
        {
            ma_sound* sound = it->second.Sound;
            if (!ma_sound_is_playing(sound) && ma_sound_at_end(sound))
            {
                ma_sound_uninit(sound);
                delete sound;
                it = s_Voices.erase(it);
            }
            else
            {
                it->second.Info.Playing = ma_sound_is_playing(sound) == MA_TRUE;
                ++it;
            }
        }
    }

    SoundHandle MiniAudioBackend::Play(const std::string& filepath, const SoundOptions& options)
    {
        if (!s_Initialized || filepath.empty())
            return kInvalidSound;

        const std::filesystem::path resolved = ResolveAssetPath(filepath);

        ma_sound* sound = new ma_sound;
        ma_result result = ma_sound_init_from_file(&s_AudioEngine, resolved.string().c_str(), 0, nullptr, nullptr,
                                                   sound);

        if (result != MA_SUCCESS)
        {
            UHE_ERROR("Failed to load sound: {}", resolved.string());
            delete sound;
            return kInvalidSound;
        }

        const SoundHandle handle = s_NextHandle++;
        ma_sound_set_spatialization_enabled(sound, options.Spatial ? MA_TRUE : MA_FALSE);
        ma_sound_set_volume(sound, options.Volume);
        ma_sound_set_pitch(sound, options.Pitch);
        ma_sound_set_pan(sound, options.Pan);
        ma_sound_set_looping(sound, options.Loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_min_distance(sound, options.MinDistance);
        ma_sound_set_max_distance(sound, options.MaxDistance);
        ma_sound_set_rolloff(sound, options.Rolloff);
        if (options.Spatial)
            ma_sound_set_position(sound, options.Position.x, options.Position.y, options.Position.z);

        ma_sound_start(sound);

        UHE_TRACE("Audio: playing '{}' (handle {})", filepath, handle);
        Voice voice;
        voice.Sound = sound;
        voice.Info.Handle = handle;
        voice.Info.Path = filepath;
        voice.Info.Volume = options.Volume;
        voice.Info.Pitch = options.Pitch;
        voice.Info.Spatial = options.Spatial;
        voice.Info.Looping = options.Loop;
        voice.Info.Playing = true;
        voice.Info.Position = options.Position;
        s_Voices.emplace(handle, std::move(voice));
        return handle;
    }

    void MiniAudioBackend::Stop(SoundHandle handle)
    {
        DestroyVoice(handle);
    }

    void MiniAudioBackend::Pause(SoundHandle handle)
    {
        if (Voice* voice = FindVoice(handle))
        {
            ma_sound_stop(voice->Sound); // keeps the cursor, unlike uninit
            voice->Info.Playing = false;
        }
    }

    void MiniAudioBackend::Resume(SoundHandle handle)
    {
        if (Voice* voice = FindVoice(handle))
        {
            ma_sound_start(voice->Sound);
            voice->Info.Playing = true;
        }
    }

    void MiniAudioBackend::StopAll()
    {
        for (auto& [handle, voice] : s_Voices)
        {
            ma_sound_stop(voice.Sound);
            ma_sound_uninit(voice.Sound);
            delete voice.Sound;
        }
        s_Voices.clear();
    }

    bool MiniAudioBackend::IsPlaying(SoundHandle handle)
    {
        Voice* voice = FindVoice(handle);
        return voice && ma_sound_is_playing(voice->Sound) == MA_TRUE;
    }

    bool MiniAudioBackend::IsValid(SoundHandle handle)
    {
        return FindVoice(handle) != nullptr;
    }

    void MiniAudioBackend::SetVolume(SoundHandle handle, float volume)
    {
        if (Voice* voice = FindVoice(handle))
        {
            ma_sound_set_volume(voice->Sound, volume);
            voice->Info.Volume = volume;
        }
    }

    void MiniAudioBackend::SetPitch(SoundHandle handle, float pitch)
    {
        if (Voice* voice = FindVoice(handle))
        {
            ma_sound_set_pitch(voice->Sound, pitch);
            voice->Info.Pitch = pitch;
        }
    }

    void MiniAudioBackend::SetPan(SoundHandle handle, float pan)
    {
        if (Voice* voice = FindVoice(handle))
            ma_sound_set_pan(voice->Sound, pan);
    }

    void MiniAudioBackend::SetPosition(SoundHandle handle, const glm::vec3& position)
    {
        if (Voice* voice = FindVoice(handle))
        {
            ma_sound_set_position(voice->Sound, position.x, position.y, position.z);
            voice->Info.Position = position;
        }
    }

    void MiniAudioBackend::SetLooping(SoundHandle handle, bool loop)
    {
        if (Voice* voice = FindVoice(handle))
        {
            ma_sound_set_looping(voice->Sound, loop ? MA_TRUE : MA_FALSE);
            voice->Info.Looping = loop;
        }
    }

    void MiniAudioBackend::SetMinMaxDistance(SoundHandle handle, float minDistance, float maxDistance)
    {
        if (Voice* voice = FindVoice(handle))
        {
            ma_sound_set_min_distance(voice->Sound, minDistance);
            ma_sound_set_max_distance(voice->Sound, maxDistance);
        }
    }

    void MiniAudioBackend::SetRolloff(SoundHandle handle, float rolloff)
    {
        if (Voice* voice = FindVoice(handle))
            ma_sound_set_rolloff(voice->Sound, rolloff);
    }

    void MiniAudioBackend::SetMasterVolume(float volume)
    {
        s_MasterVolume = std::clamp(volume, 0.0f, 4.0f);
        if (s_Initialized)
            ma_engine_set_volume(&s_AudioEngine, s_MasterVolume);
    }

    float MiniAudioBackend::GetMasterVolume()
    {
        return s_MasterVolume;
    }

    void MiniAudioBackend::SetListenerPosition(const glm::vec3& position, const glm::vec3& forward,
                                               const glm::vec3& up)
    {
        if (!s_Initialized)
            return;

        ma_engine_listener_set_position(&s_AudioEngine, 0, position.x, position.y, position.z);
        ma_engine_listener_set_direction(&s_AudioEngine, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&s_AudioEngine, 0, up.x, up.y, up.z);
    }

    std::vector<ActiveVoice> MiniAudioBackend::GetActiveVoices()
    {
        std::vector<ActiveVoice> voices;
        voices.reserve(s_Voices.size());
        for (const auto& [handle, voice] : s_Voices)
            voices.push_back(voice.Info);
        return voices;
    }
} // namespace UHE::Audio
