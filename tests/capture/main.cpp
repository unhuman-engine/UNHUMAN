// Capture harness: the environment hook behind the golden-image check parses
// strictly (a typo never captures frame 0 or freezes the clock), and the image
// comparison passes only what it should: identical frames, rounding-level
// noise, and nothing that is a real rendering change.

#include <iostream>
#include <string>
#include "ImageDiff.h"
#include "UHE/Core/CaptureSettings.h"

using UHE::CaptureSettings;
namespace Diff = UHE::ImageDiff;

namespace
{
int g_failures = 0;

void check(bool condition, const std::string& what)
{
    std::cout << (condition ? "  ok   - " : "  FAIL - ") << what << '\n';
    g_failures += condition ? 0 : 1;
}

void section(const char* name)
{
    std::cout << "\n[ " << name << " ]\n";
}

Diff::Image Solid(int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    Diff::Image image;
    image.Width = width;
    image.Height = height;
    for (std::size_t i = 0; i < std::size_t(width) * height; ++i)
        image.Rgba.insert(image.Rgba.end(), {r, g, b, 255});
    return image;
}

void SetPixel(Diff::Image& image, std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    image.Rgba[index * 4 + 0] = r;
    image.Rgba[index * 4 + 1] = g;
    image.Rgba[index * 4 + 2] = b;
}
} // namespace

int main()
{
    section("capture settings: unset");
    {
        const auto s = CaptureSettings::Parse(nullptr, nullptr, nullptr);
        check(!s.CaptureEnabled() && !s.FixedTimestep, "nothing set: no capture, wall clock");
        check(s.CapturePath == "capture.png", "the default path is capture.png");
    }

    section("capture settings: valid values");
    {
        const auto s = CaptureSettings::Parse("0.016666667", "30", "out/frame.png");
        check(s.FixedTimestep && *s.FixedTimestep > 0.0166f && *s.FixedTimestep < 0.0167f, "a fixed step parses");
        check(s.CaptureEnabled() && *s.CaptureFrame == 30, "a capture frame parses");
        check(s.CapturePath == "out/frame.png", "the path is taken as given");
        check(CaptureSettings::Parse(nullptr, "1", nullptr).CaptureFrame == 1u, "frame 1 is the first frame");
    }

    section("capture settings: malformed values are ignored");
    {
        check(!CaptureSettings::Parse(nullptr, "0", nullptr).CaptureEnabled(), "frame 0 does not exist");
        check(!CaptureSettings::Parse(nullptr, "-3", nullptr).CaptureEnabled(), "a negative frame is refused");
        check(!CaptureSettings::Parse(nullptr, "30x", nullptr).CaptureEnabled(), "trailing junk is refused");
        check(!CaptureSettings::Parse(nullptr, "", nullptr).CaptureEnabled(), "an empty value is unset");
        check(!CaptureSettings::Parse(nullptr, "99999999999", nullptr).CaptureEnabled(), "overflow is refused");
        check(!CaptureSettings::Parse("0", nullptr, nullptr).FixedTimestep, "a zero step would freeze the clock");
        check(!CaptureSettings::Parse("-0.1", nullptr, nullptr).FixedTimestep, "a negative step is refused");
        check(!CaptureSettings::Parse("fast", nullptr, nullptr).FixedTimestep, "a word is not a step");
        check(!CaptureSettings::Parse("inf", nullptr, nullptr).FixedTimestep, "infinity is not a step");
        check(CaptureSettings::Parse(nullptr, nullptr, "").CapturePath == "capture.png",
              "an empty path keeps the default");
    }

    const Diff::Policy policy; // tolerance 8, at most 0.1% of pixels

    section("image diff: identical and near-identical");
    {
        const auto a = Solid(100, 100, 120, 10, 10);
        const auto r = Diff::Compare(a, a, policy);
        check(r.Matches(policy) && r.DifferingPixels == 0 && r.MaxChannelDelta == 0, "an image matches itself");
        check(r.Diff.Width == 100 && r.Diff.Rgba.size() == a.Rgba.size(), "the diff image has the input's size");

        auto b = a;
        for (int i = 0; i < 100 * 100; ++i)
            SetPixel(b, i, 127, 14, 6); // every channel within 8
        const auto noise = Diff::Compare(a, b, policy);
        check(noise.Matches(policy) && noise.DifferingPixels == 0, "rounding-level noise everywhere still matches");
        check(noise.MaxChannelDelta == 7, "and reports the largest delta");
    }

    section("image diff: a few edge pixels");
    {
        const auto a = Solid(100, 100, 0, 0, 0);
        auto b = a;
        for (int i = 0; i < 10; ++i) // 0.1% of 10 000
            SetPixel(b, i, 255, 255, 255);
        check(Diff::Compare(a, b, policy).Matches(policy), "differences up to the ratio limit pass");
        SetPixel(b, 10, 255, 255, 255);
        const auto r = Diff::Compare(a, b, policy);
        check(!r.Matches(policy) && r.DifferingPixels == 11, "one pixel over the limit fails");
        check(r.Diff.Rgba[0] == 255 && r.Diff.Rgba[1] == 0, "a differing pixel is red in the diff");
        check(r.Diff.Rgba[std::size_t{11} * 4] != 255, "a matching pixel is not");
    }

    section("image diff: real changes");
    {
        const auto red = Solid(64, 64, 200, 0, 0);
        const auto blue = Solid(64, 64, 0, 0, 200);
        check(!Diff::Compare(red, blue, policy).Matches(policy), "swapped channels fail (a BGRA/RGBA mix-up)");
        check(!Diff::Compare(red, Solid(64, 64, 0, 0, 0), policy).Matches(policy), "a black frame fails");
        const auto r = Diff::Compare(red, Solid(64, 32, 200, 0, 0), policy);
        check(r.SizeMismatch && !r.Matches(policy), "a different size fails without comparing");
        Diff::Image truncated = red;
        truncated.Rgba.resize(10);
        check(Diff::Compare(red, truncated, policy).SizeMismatch, "a short buffer is a size mismatch, not a crash");
    }

    std::cout << '\n' << (g_failures == 0 ? "all capture checks passed" : "capture checks FAILED") << '\n';
    return g_failures == 0 ? 0 : 1;
}
