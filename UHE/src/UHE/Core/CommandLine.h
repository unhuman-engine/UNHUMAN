#pragma once

#include "UHE/Core/Core.h"

#include <string>
#include <vector>

namespace UHE
{
    /**
     * @brief Process arguments, captured once by EntryPoint's main().
     *
     * Options accept both "-name" and "--name"; values are either the following
     * argument ("-project /path") or attached with '=' ("-project=/path").
     * argv[0] (the executable path) is not stored.
     */
    class UHE_API CommandLine
    {
    public:
        static void Init(int argc, char** argv);

        [[nodiscard]] static const std::vector<std::string>& GetArgs();

        [[nodiscard]] static bool HasFlag(const char* name);

        // Empty when the option is absent or has no value.
        [[nodiscard]] static std::string GetValue(const char* name);
    };
} // namespace UHE
