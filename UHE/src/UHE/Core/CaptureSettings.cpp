#include "CaptureSettings.h"
#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace UHE
{

namespace
{
/// @brief Whole-string float parse; nullopt for anything strtof does not consume fully.
std::optional<float> ParsePositiveFloat(const char* text)
{
    if (text == nullptr || *text == '\0')
        return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(text, &end);
    if (errno != 0 || *end != '\0' || !std::isfinite(value) || value <= 0.0f)
        return std::nullopt;
    return value;
}

/// @brief Whole-string decimal parse of a value in [1, UINT32_MAX].
std::optional<u32> ParsePositiveInt(const char* text)
{
    if (text == nullptr || *text == '\0' || *text == '-' || *text == '+')
        return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > 0xFFFFFFFFull)
        return std::nullopt;
    return static_cast<u32>(value);
}
} // namespace

CaptureSettings CaptureSettings::Parse(const char* fixedTimestep, const char* captureFrame, const char* capturePath)
{
    CaptureSettings settings;
    settings.FixedTimestep = ParsePositiveFloat(fixedTimestep);
    settings.CaptureFrame = ParsePositiveInt(captureFrame);
    if (capturePath != nullptr && *capturePath != '\0')
        settings.CapturePath = capturePath;
    return settings;
}

CaptureSettings CaptureSettings::FromEnvironment()
{
    return Parse(std::getenv("UHE_FIXED_TIMESTEP"), std::getenv("UHE_CAPTURE_FRAME"), std::getenv("UHE_CAPTURE_PATH"));
}

} // namespace UHE
