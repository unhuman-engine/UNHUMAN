#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

/**
 * @file ImageDiff.h
 * @brief Pixel comparison behind the golden-image check (`uhe_imgdiff`).
 *
 * Header-only and dependency-free so tests/capture can exercise the policy
 * without decoding a PNG.
 */
namespace UHE::ImageDiff
{

/// @brief Tightly packed RGBA8, top row first.
struct Image
{
    int Width = 0;
    int Height = 0;
    std::vector<std::uint8_t> Rgba;
};

/// @brief How far two images may differ and still match.
struct Policy
{
    /// A pixel differs when any channel is more than this apart (0-255).
    /// Absorbs rounding a driver update may change; a real rendering change
    /// moves channels by far more.
    int ChannelTolerance = 8;
    /// The images match when at most this fraction of pixels differ.
    /// Absorbs edge pixels whose coverage a rasteriser rounds differently.
    double MaxDifferingRatio = 0.001;
};

/// @brief Outcome of Compare().
struct Result
{
    bool SizeMismatch = false;
    std::size_t DifferingPixels = 0;
    std::size_t TotalPixels = 0;
    int MaxChannelDelta = 0;
    /// Same size as the inputs: differing pixels red, the rest a dimmed grey of \p expected.
    Image Diff;

    [[nodiscard]] double DifferingRatio() const
    {
        return TotalPixels == 0 ? 0.0 : static_cast<double>(DifferingPixels) / static_cast<double>(TotalPixels);
    }
    [[nodiscard]] bool Matches(const Policy& policy) const
    {
        return !SizeMismatch && DifferingRatio() <= policy.MaxDifferingRatio;
    }
};

/// @brief Compares \p actual against \p expected pixel by pixel under \p policy.
[[nodiscard]] inline Result Compare(const Image& expected, const Image& actual, const Policy& policy)
{
    Result result;
    const std::size_t expectedBytes = static_cast<std::size_t>(expected.Width) * expected.Height * 4;
    if (expected.Width != actual.Width || expected.Height != actual.Height || expected.Rgba.size() != expectedBytes ||
        actual.Rgba.size() != expectedBytes)
    {
        result.SizeMismatch = true;
        return result;
    }

    result.TotalPixels = static_cast<std::size_t>(expected.Width) * expected.Height;
    result.Diff.Width = expected.Width;
    result.Diff.Height = expected.Height;
    result.Diff.Rgba.resize(expectedBytes);

    for (std::size_t i = 0; i < expectedBytes; i += 4)
    {
        int delta = 0;
        for (std::size_t c = 0; c < 4; ++c)
            delta = std::max(delta, std::abs(int(expected.Rgba[i + c]) - int(actual.Rgba[i + c])));
        result.MaxChannelDelta = std::max(result.MaxChannelDelta, delta);

        std::uint8_t* out = &result.Diff.Rgba[i];
        if (delta > policy.ChannelTolerance)
        {
            ++result.DifferingPixels;
            out[0] = 255;
            out[1] = 0;
            out[2] = 0;
        }
        else
        {
            const int grey = (expected.Rgba[i] + expected.Rgba[i + 1] + expected.Rgba[i + 2]) / 3;
            out[0] = out[1] = out[2] = static_cast<std::uint8_t>(grey / 3);
        }
        out[3] = 255;
    }
    return result;
}

} // namespace UHE::ImageDiff
