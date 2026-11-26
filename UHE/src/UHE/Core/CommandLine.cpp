#include "uhepch.h"
#include "CommandLine.h"

namespace UHE
{
namespace
{
std::vector<std::string> s_Args;
std::string s_Executable; ///< argv[0]

// Normalizes "-x" / "--x" to "x" so both spellings match.
std::string_view NormalizeFlag(std::string_view arg)
{
    while (!arg.empty() && arg.front() == '-')
        arg.remove_prefix(1);
    return arg;
}
} // namespace

void CommandLine::Init(int argc, char** argv)
{
    s_Args.clear();
    s_Executable = argc > 0 && argv != nullptr && argv[0] != nullptr ? argv[0] : "";
    if (argc <= 1 || argv == nullptr)
        return;

    s_Args.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] != nullptr)
            s_Args.emplace_back(argv[i]);
    }
}

const std::string& CommandLine::GetExecutable()
{
    return s_Executable;
}

std::filesystem::path CommandLine::GetExecutablePath()
{
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size())
        return std::filesystem::path(buffer.substr(0, length));
#elif defined(__linux__)
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !self.empty())
        return self;
#endif
    if (s_Executable.empty())
        return {};
    return std::filesystem::absolute(s_Executable, ec);
}

const std::vector<std::string>& CommandLine::GetArgs()
{
    return s_Args;
}

bool CommandLine::HasFlag(const char* name)
{
    if (name == nullptr)
        return false;
    const std::string_view wanted = NormalizeFlag(name);
    for (const std::string& arg : s_Args)
    {
        std::string_view view = arg;
        if (const std::size_t equals = view.find('='); equals != std::string_view::npos)
            view = view.substr(0, equals);
        if (NormalizeFlag(view) == wanted)
            return true;
    }
    return false;
}

std::string CommandLine::GetValue(const char* name)
{
    if (name == nullptr)
        return {};
    const std::string_view wanted = NormalizeFlag(name);

    for (std::size_t i = 0; i < s_Args.size(); ++i)
    {
        const std::string_view view = s_Args[i];
        const std::size_t equals = view.find('=');
        const std::string_view flag = (equals == std::string_view::npos) ? view : view.substr(0, equals);
        if (NormalizeFlag(flag) != wanted)
            continue;

        if (equals != std::string_view::npos)
            return std::string(view.substr(equals + 1));
        if (i + 1 < s_Args.size())
            return s_Args[i + 1];
        return {};
    }
    return {};
}
} // namespace UHE
