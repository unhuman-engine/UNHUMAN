#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace UHE::RHI::VULKAN::Validation
{
/**
 * \brief The validation gate's policy, with no Vulkan dependency.
 *
 * `VulkanInstance::debugCallback` is a driver callback that runs once per
 * message, so what it decides -- what counts as an error, when to stop, what to
 * do with the thousandth copy of the same defect -- lives here instead, where it
 * can be tested without an instance, a device or a display.
 */

/// Severity bits, a mirror of `VkDebugUtilsMessageSeverityFlagBitsEXT`.
///
/// Mirrored rather than included so this header stays Vulkan-free;
/// `VulkanInstance.cpp` static_asserts that the two agree, so they cannot drift.
enum SeverityFlag : unsigned
{
    Verbose = 0x00000001,
    Info = 0x00000010,
    Warning = 0x00000100,
    Error = 0x00001000,
};

/// The one class a severity bitmask is reduced to.
enum class Severity
{
    Verbose,
    Info,
    Warning,
    Error,
};

/// \returns the class of a severity bitmask.
///
/// The highest set class wins, and a mask with no known bit is `Verbose`: an
/// unrecognised severity is not an error, because stopping a run on one would
/// turn a layer quirk into a build failure.
[[nodiscard]] Severity Classify(unsigned severityFlags);

/// \returns whether the value of an on/off environment variable means on.
///
/// Unset, empty, `0`, `false`, `off` and `no` (any case, surrounding spaces
/// ignored) are off; anything else is on. A variable that is merely *set* is not
/// automatically a yes: `UHE_VALIDATION_FATAL=0` is how a caller says "off", and
/// reading it as on would abort the run on the first validation error anyway.
[[nodiscard]] bool ParseEnabledFlag(const char* value);

/// \returns whether `UHE_VALIDATION_FATAL` asks for a validation error to stop the process.
///
/// Read once, on the first call: this is consulted per validation message, and
/// the answer cannot change mid-run.
[[nodiscard]] bool FatalValidationEnabled();

/// \returns a stable identity for a validation message, for repeat detection.
///
/// Keyed on the layer's id rather than on the message text, which embeds object
/// handles and so differs between two reports of the same defect. Empty when the
/// message has neither an id name nor a number: what cannot be identified is
/// never treated as a repeat of something else.
[[nodiscard]] std::string MessageKey(const char* idName, std::int32_t idNumber);

/// What to do with a message that has been seen before.
enum class RepeatVerdict
{
    First,      ///< Not seen before: log it in full.
    Suppressed, ///< Seen before, and this repeat is not worth a line.
    Report,     ///< Seen before, and this repeat count is worth a line.
};

/**
 * \brief Collapses a flood of identical messages into a bounded number of lines.
 *
 * The layer reports the same defect once per frame, so an unfiltered log buries
 * the first and most useful occurrence under thousands of copies of itself --
 * the tail a failing CI job prints is then all repeats. The first occurrence is
 * always logged; later ones are silent except at 2, 4, 8 ... times, which bounds
 * the noise at log2(N) lines per distinct message.
 *
 * Safe to call from the callback's threads: the layer may report from more than
 * one.
 */
class RepeatFilter
{
public:
    /// \param key a `MessageKey`; an empty key is always reported as `First`.
    RepeatVerdict Observe(std::string_view key);

    /// \returns how many times `key` has been observed, 0 when never.
    [[nodiscard]] unsigned Count(std::string_view key) const;

    /// \returns the number of distinct messages being tracked.
    [[nodiscard]] std::size_t Size() const;

private:
    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, unsigned> m_Counts;
};
} // namespace UHE::RHI::VULKAN::Validation
