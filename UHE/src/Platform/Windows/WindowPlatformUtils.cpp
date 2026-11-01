#ifdef _WIN32
#include "uhepch.h"
#include <GLFW/glfw3.h>
#include "UHE/Utils/PlatfromUtils.h"
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include "UHE/Core/Application.h"

namespace UHE
{
std::string FileDialogs::OpenFile(const char* filter, const std::string& defaultPath)
{
    OPENFILENAMEA ofn;
    CHAR szFile[260] = {0};
    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = glfwGetWin32Window((GLFWwindow*)Application::Get().GetWindow().GetNativeWindow());
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter;
    ofn.lpstrInitialDir = defaultPath.empty() ? nullptr : defaultPath.c_str();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn) == TRUE)
    {
        return ofn.lpstrFile;
    }
    return std::string();
};
std::string FileDialogs::SaveFile(const char* filter, const std::string& defaultPath)
{
    OPENFILENAMEA ofn;
    CHAR szFile[260] = {0};
    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = glfwGetWin32Window((GLFWwindow*)Application::Get().GetWindow().GetNativeWindow());
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter;
    ofn.lpstrInitialDir = defaultPath.empty() ? nullptr : defaultPath.c_str();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetSaveFileNameA(&ofn) == TRUE)
    {
        return ofn.lpstrFile;
    }
    return std::string();
};

std::string FileDialogs::SelectFolder(const char* title, const std::string& defaultPath)
{
    // BROWSEINFO has no initial-directory member, so the start folder is applied
    // from the dialog's own initialisation callback.
    static std::string s_initialFolder;
    s_initialFolder = defaultPath;

    BROWSEINFOA bi;
    ZeroMemory(&bi, sizeof(BROWSEINFOA));
    bi.hwndOwner = glfwGetWin32Window((GLFWwindow*)Application::Get().GetWindow().GetNativeWindow());
    bi.lpszTitle = title != nullptr ? title : "Select Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = [](HWND hwnd, UINT message, LPARAM, LPARAM) -> int {
        if (message == BFFM_INITIALIZED && !s_initialFolder.empty())
            SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, reinterpret_cast<LPARAM>(s_initialFolder.c_str()));
        return 0;
    };

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl == nullptr)
        return std::string();

    CHAR path[MAX_PATH] = {0};
    const BOOL ok = SHGetPathFromIDListA(pidl, path);
    CoTaskMemFree(pidl);
    return ok == TRUE ? std::string(path) : std::string();
};


bool FileDialogs::OpenInDefaultApp(const std::string& path)
{
    if (path.empty())
        return false;

    const HINSTANCE result = ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<std::intptr_t>(result) > 32; // ShellExecute's legacy "success" range
}
} // namespace UHE
#endif
