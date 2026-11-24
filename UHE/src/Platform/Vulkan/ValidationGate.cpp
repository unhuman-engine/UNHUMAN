#include "Platform/Vulkan/ValidationGate.h"
#include <cctype>
#include <cstdlib>

namespace UHE::RHI::VULKAN::Validation
{
namespace
{
/// Whether `value` equals `word`, ignoring ASCII case.
bool EqualsIgnoreCase(std::string_view value, std::string_view word)
{
    if (value.size() != word.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const auto left = static_cast<unsigned char>(value[i]);
        const auto right = static_cast<unsigned char>(word[i]);
        if (std::tolower(left) != std::tolower(right))
        {
            return false;
        }
    }
    return true;
}
} // namespace

Severity Classify(unsigned severityFlags)
{
    if (severityFlags & Error)
    {
        return Severity::Error;
    }
    if (severityFlags & Warning)
    {
        return Severity::Warning;
    }
    if (severityFlags & Info)
    {
        return Severity::Info;
    }
    return Severity::Verbose;
}

bool ParseEnabledFlag(const char* value)
{
    if (value == nullptr)
    {
        return false;
    }

    std::string_view text(value);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
    {
        text.remove_suffix(1);
    }

    if (text.empty() || EqualsIgnoreCase(text, "0") || EqualsIgnoreCase(text, "false") ||
        EqualsIgnoreCase(text, "off") || EqualsIgnoreCase(text, "no"))
    {
        return false;
    }
    return true;
}

bool FatalValidationEnabled()
{
    static const bool enabled = ParseEnabledFlag(std::getenv("UHE_VALIDATION_FATAL"));
    return enabled;
}

std::string MessageKey(const char* idName, std::int32_t idNumber)
{
    const bool hasName = idName != nullptr && *idName != '\0';
    if (!hasName && idNumber == 0)
    {
        return {};
    }

    std::string key = hasName ? idName : "";
    key += '#';
    key += std::to_string(idNumber);
    return key;
}

RepeatVerdict RepeatFilter::Observe(std::string_view key)
{
    if (key.empty())
    {
        return RepeatVerdict::First;
    }

    const std::lock_guard lock(m_Mutex);
    unsigned& seen = m_Counts[std::string(key)];
    ++seen;

    if (seen == 1)
    {
        return RepeatVerdict::First;
    }
    // 2, 4, 8 ... : log2(N) lines for N reports, so the flood stays readable.
    if ((seen & (seen - 1)) == 0)
    {
        return RepeatVerdict::Report;
    }
    return RepeatVerdict::Suppressed;
}

unsigned RepeatFilter::Count(std::string_view key) const
{
    const std::lock_guard lock(m_Mutex);
    const auto it = m_Counts.find(std::string(key));
    return it == m_Counts.end() ? 0u : it->second;
}

std::size_t RepeatFilter::Size() const
{
    const std::lock_guard lock(m_Mutex);
    return m_Counts.size();
}
} // namespace UHE::RHI::VULKAN::Validation
