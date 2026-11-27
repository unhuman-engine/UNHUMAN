#pragma once
/**
 * @file CookedAudio.h
 * @brief WAV sounds cooked to PCM the runtime plays without parsing WAV (issue #45).
 *
 * An export cooks `<sound>.wav` into `<sound>.wav.uheaudio`: a cooked file
 * (UHE/AssetsManager/CookedFile.h) holding the samples as 32-bit floats,
 * zstd-compressed, with their channel count and rate. MiniAudioBackend plays
 * it from memory when the WAV is not shipped. OGG and MP3 stay as they are:
 * they are compressed streams the player decodes, and cooking them to PCM
 * would multiply a game's size.
 */
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE
{

struct AudioPcm
{
    u32 Channels = 0;
    u32 SampleRate = 0;
    std::vector<float> Samples; ///< interleaved, Channels per frame, in [-1, 1]

    [[nodiscard]] u64 Frames() const { return Channels == 0 ? 0 : Samples.size() / Channels; }
};

/// @brief Decodes a RIFF WAVE file: 8/16/24/32-bit integer or 32-bit float PCM, any channel count.
UHE_API bool DecodeWav(std::span<const u8> bytes, AudioPcm& out, std::string& error);

UHE_API bool SaveCookedAudio(const std::filesystem::path& path, const AudioPcm& pcm, u64 sourceHash,
                             std::string& error);
UHE_API bool LoadCookedAudio(const std::filesystem::path& path, AudioPcm& out, std::string& error);

/// @brief The cook beside a source: `<source>.uheaudio`.
[[nodiscard]] UHE_API std::filesystem::path CookedAudioPathFor(const std::filesystem::path& source);

/// @brief Cooks the WAV at \p source into \p cooked (ExportOptions::CookAudio); false with a reason.
UHE_API bool CookAudioFile(const std::filesystem::path& source, const std::filesystem::path& cooked,
                           std::string& error);

} // namespace UHE
