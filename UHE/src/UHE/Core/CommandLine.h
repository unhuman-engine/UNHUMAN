#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "UHE/Core/Core.h"

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

    /// @brief The arguments after the executable's own path.
    [[nodiscard]] static const std::vector<std::string>& GetArgs();
    /// @brief argv[0]: how the executable was started (relative or absolute); empty before Init.
    [[nodiscard]] static const std::string& GetExecutable();
    /**
     * @brief The running executable's absolute path, from the OS (/proc/self/exe,
     *        GetModuleFileName); argv[0] made absolute where the OS does not say.
     *
     * argv[0] alone is only the name when the program was found through PATH,
     * and resolving that against the working directory finds the wrong folder.
     */
    [[nodiscard]] static std::filesystem::path GetExecutablePath();

    [[nodiscard]] static bool HasFlag(const char* name);

    // Empty when the option is absent or has no value.
    [[nodiscard]] static std::string GetValue(const char* name);
};
} // namespace UHE
