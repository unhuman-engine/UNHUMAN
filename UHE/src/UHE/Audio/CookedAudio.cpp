#include "uhepch.h"
#include "UHE/Audio/CookedAudio.h"
#include <cstring>
#include <fstream>
#include <iterator>
#include "UHE/AssetsManager/CookedFile.h"

namespace UHE
{
namespace
{
constexpr u32 kAssetAudio = Cooked::FourCC("AUDI");
constexpr u32 kSectionInfo = Cooked::FourCC("INFO");
constexpr u32 kSectionSamples = Cooked::FourCC("PCMF");
constexpr u32 kAudioVersion = 1;

u32 ReadU32(const u8* p)
{
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

u16 ReadU16(const u8* p)
{
    return static_cast<u16>(p[0] | (p[1] << 8));
}
} // namespace

bool DecodeWav(std::span<const u8> bytes, AudioPcm& out, std::string& error)
{
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    {
        error = "not a RIFF WAVE file";
        return false;
    }
    u16 format = 0, channels = 0, bits = 0;
    u32 rate = 0;
    const u8* data = nullptr;
    std::size_t dataSize = 0;
    // Chunks after the header: "fmt " and "data" are what matter, in any order.
    for (std::size_t at = 12; at + 8 <= bytes.size();)
    {
        const u32 size = ReadU32(&bytes[at + 4]);
        const std::size_t body = at + 8;
        if (size > bytes.size() - body)
        {
            error = "a chunk runs past the end of the file";
            return false;
        }
        if (std::memcmp(&bytes[at], "fmt ", 4) == 0 && size >= 16)
        {
            format = ReadU16(&bytes[body]);
            channels = ReadU16(&bytes[body + 2]);
            rate = ReadU32(&bytes[body + 4]);
            bits = ReadU16(&bytes[body + 14]);
            if (format == 0xFFFE && size >= 26) // WAVE_FORMAT_EXTENSIBLE: the sub-format's tag
                format = ReadU16(&bytes[body + 24]);
        }
        else if (std::memcmp(&bytes[at], "data", 4) == 0)
        {
            data = &bytes[body];
            dataSize = size;
        }
        at = body + size + (size & 1); // chunks are padded to an even size
    }
    const bool integer = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool floating = format == 3 && bits == 32;
    if (data == nullptr || channels == 0 || rate == 0 || (!integer && !floating))
    {
        error = "unsupported WAVE (format " + std::to_string(format) + ", " + std::to_string(bits) + " bits)";
        return false;
    }
    const std::size_t stride = bits / 8;
    const std::size_t count = dataSize / stride / channels * channels;
    out.Channels = channels;
    out.SampleRate = rate;
    out.Samples.resize(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const u8* p = data + (i * stride);
        float sample = 0.0f;
        if (floating)
            std::memcpy(&sample, p, 4);
        else if (bits == 8)
            sample = (static_cast<float>(p[0]) - 128.0f) / 128.0f; // 8-bit WAV is unsigned
        else if (bits == 16)
            sample = static_cast<float>(static_cast<i16>(ReadU16(p))) / 32768.0f;
        else if (bits == 24)
        {
            const i32 value = static_cast<i32>((static_cast<u32>(p[0]) << 8) | (static_cast<u32>(p[1]) << 16) |
                                               (static_cast<u32>(p[2]) << 24)) >>
                              8;
            sample = static_cast<float>(value) / 8388608.0f;
        }
        else
            sample = static_cast<float>(static_cast<i32>(ReadU32(p))) / 2147483648.0f;
        out.Samples[i] = sample;
    }
    return true;
}

bool SaveCookedAudio(const std::filesystem::path& path, const AudioPcm& pcm, u64 sourceHash, std::string& error)
{
    Cooked::Writer file;
    Cooked::BinaryWriter info;
    info.Put(kAudioVersion);
    info.Put(pcm.Channels);
    info.Put(pcm.SampleRate);
    info.Put<u64>(pcm.Frames());
    file.AddSection(kSectionInfo, info.Take(), false);
    Cooked::BinaryWriter samples;
    samples.PutVector(pcm.Samples);
    file.AddSection(kSectionSamples, samples.Take());
    return file.Write(path, kAssetAudio, sourceHash, &error);
}

bool LoadCookedAudio(const std::filesystem::path& path, AudioPcm& out, std::string& error)
{
    Cooked::Reader file;
    if (!file.Open(path, &error))
        return false;
    if (file.AssetType() != kAssetAudio)
    {
        error = "a cooked " + Cooked::FourCCName(file.AssetType()) + ", not audio";
        return false;
    }
    std::vector<u8> bytes;
    if (!file.ReadSection(kSectionInfo, bytes, &error))
        return false;
    Cooked::BinaryReader info(bytes);
    const u32 version = info.Get<u32>();
    out.Channels = info.Get<u32>();
    out.SampleRate = info.Get<u32>();
    const u64 frames = info.Get<u64>();
    if (!info.Ok() || version != kAudioVersion || out.Channels == 0 || out.Channels > 64 || out.SampleRate == 0)
    {
        error = "damaged or newer INFO section; recook it";
        return false;
    }
    if (!file.ReadSection(kSectionSamples, bytes, &error))
        return false;
    Cooked::BinaryReader samples(bytes);
    if (!samples.GetVector(out.Samples) || out.Samples.size() != frames * out.Channels)
    {
        error = "damaged PCMF section";
        return false;
    }
    return true;
}

std::filesystem::path CookedAudioPathFor(const std::filesystem::path& source)
{
    return source.string() + ".uheaudio";
}

bool CookAudioFile(const std::filesystem::path& source, const std::filesystem::path& cooked, std::string& error)
{
    std::ifstream in(source, std::ios::binary);
    if (!in)
    {
        error = "it cannot be read";
        return false;
    }
    const std::vector<u8> bytes(std::istreambuf_iterator<char>(in), {});
    AudioPcm pcm;
    return DecodeWav(bytes, pcm, error) && SaveCookedAudio(cooked, pcm, Cooked::HashFile(source), error);
}

} // namespace UHE
