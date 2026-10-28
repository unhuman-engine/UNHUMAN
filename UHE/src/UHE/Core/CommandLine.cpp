#include "uhepch.h"
#include "CommandLine.h"

namespace UHE
{
    namespace
    {
        std::vector<std::string> s_Args;

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
        if (argc <= 1 || argv == nullptr)
            return;

        s_Args.reserve(static_cast<std::size_t>(argc - 1));
        for (int i = 1; i < argc; ++i)
        {
            if (argv[i] != nullptr)
                s_Args.emplace_back(argv[i]);
        }
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
