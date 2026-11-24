#ifdef __linux__
    #include "uhepch.h"
    #include <array>
    #include <cstdio>
    #include <memory>
    #include <string>
    #include <sys/wait.h>
    #include <unistd.h>
    #include "UHE/Utils/PlatfromUtils.h"

namespace UHE {
namespace
{
// zenity prints the chosen path on stdout; an empty result means the user
// cancelled (or zenity is not installed).
std::string RunZenity(const std::string& arguments)
{
    std::array<char, 128> buffer;
    std::string result;
    // NOLINTNEXTLINE(bugprone-command-processor): running zenity is the point; arguments are built here.
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(("zenity " + arguments).c_str(), "r"), pclose);
    if (!pipe)
        return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    {
        result += buffer.data();
    }
    if (!result.empty() && result.back() == '\n')
        result.pop_back();
    return result;
}

std::string StartingDirectory(const std::string& defaultPath)
{
    if (defaultPath.empty())
        return "";
    return " --filename=\"" + defaultPath + "\"";
}
} // namespace

std::string FileDialogs::OpenFile(const char* filter, const std::string& defaultPath)
{
    (void)filter; // zenity filters differ from the Win32 filter string format
    return RunZenity("--file-selection --title=\"Open File\"" + StartingDirectory(defaultPath));
}

std::string FileDialogs::SaveFile(const char* filter, const std::string& defaultPath)
{
    (void)filter;
    return RunZenity("--file-selection --save --title=\"Save File\"" + StartingDirectory(defaultPath));
}

std::string FileDialogs::SelectFolder(const char* title, const std::string& defaultPath)
{
    const std::string dialogTitle = title != nullptr ? title : "Select Folder";
    return RunZenity("--file-selection --directory --title=\"" + dialogTitle + "\"" + StartingDirectory(defaultPath));
}

bool FileDialogs::OpenInDefaultApp(const std::string& path)
{
    if (path.empty())
        return false;

    // fork/exec rather than system(): the path never reaches a shell, so a
    // quote or a semicolon in a filename stays part of the filename.
    const pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0)
    {
        execlp("xdg-open", "xdg-open", path.c_str(), static_cast<char*>(nullptr));
        _exit(127); // no opener installed: report failure to the parent
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}
#endif
