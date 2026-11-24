/**
 * @file main.cpp
 * @brief uhe_imgdiff: compares a rendered frame against a golden image.
 *
 *   uhe_imgdiff <expected.png> <actual.png> [--diff <out.png>]
 *               [--tolerance <0-255>] [--max-ratio <0-1>]
 *
 * Exit codes: 0 the images match, 1 they differ, 2 usage or I/O error.
 * CI runs it on the Sandbox frame captured on lavapipe (see
 * .github/workflows/test-renderer.yml) and uploads the diff on failure.
 */
#include "ImageDiff.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

namespace
{
std::optional<UHE::ImageDiff::Image> Load(const char* path)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path, &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        std::fprintf(stderr, "uhe_imgdiff: cannot read '%s': %s\n", path, stbi_failure_reason());
        return std::nullopt;
    }
    UHE::ImageDiff::Image image;
    image.Width = width;
    image.Height = height;
    image.Rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return image;
}

/// @brief Whole-string number parse; false on junk so a typo is a usage error, not 0.
bool ParseNumber(const char* text, double& out)
{
    char* end = nullptr;
    out = std::strtod(text, &end);
    return end != text && *end == '\0';
}

int Usage()
{
    std::fprintf(stderr, "usage: uhe_imgdiff <expected.png> <actual.png> [--diff <out.png>] "
                         "[--tolerance <0-255>] [--max-ratio <0-1>]\n");
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
        return Usage();

    const char* expectedPath = argv[1];
    const char* actualPath = argv[2];
    const char* diffPath = nullptr;
    UHE::ImageDiff::Policy policy;

    for (int i = 3; i < argc; ++i)
    {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--diff") == 0 && hasValue)
            diffPath = argv[++i];
        else if (std::strcmp(argv[i], "--tolerance") == 0 && hasValue)
        {
            double value = 0.0;
            if (!ParseNumber(argv[++i], value) || value < 0.0 || value > 255.0)
                return Usage();
            policy.ChannelTolerance = static_cast<int>(value);
        }
        else if (std::strcmp(argv[i], "--max-ratio") == 0 && hasValue)
        {
            double value = 0.0;
            if (!ParseNumber(argv[++i], value) || value < 0.0 || value > 1.0)
                return Usage();
            policy.MaxDifferingRatio = value;
        }
        else
            return Usage();
    }

    const auto expected = Load(expectedPath);
    const auto actual = Load(actualPath);
    if (!expected || !actual)
        return 2;

    const UHE::ImageDiff::Result result = UHE::ImageDiff::Compare(*expected, *actual, policy);
    if (result.SizeMismatch)
    {
        std::printf("FAIL: size differs: expected %dx%d, got %dx%d\n", expected->Width, expected->Height,
                    actual->Width, actual->Height);
        return 1;
    }

    std::printf("%zu of %zu pixels differ (%.4f%%, limit %.4f%%), largest channel delta %d (tolerance %d)\n",
                result.DifferingPixels, result.TotalPixels, result.DifferingRatio() * 100.0,
                policy.MaxDifferingRatio * 100.0, result.MaxChannelDelta, policy.ChannelTolerance);

    if (diffPath != nullptr &&
        !stbi_write_png(diffPath, result.Diff.Width, result.Diff.Height, 4, result.Diff.Rgba.data(),
                        result.Diff.Width * 4))
        std::fprintf(stderr, "uhe_imgdiff: cannot write '%s'\n", diffPath);

    const bool matches = result.Matches(policy);
    std::printf("%s\n", matches ? "PASS" : "FAIL");
    return matches ? 0 : 1;
}
