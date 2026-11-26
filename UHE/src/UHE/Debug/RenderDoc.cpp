#include "uhepch.h"
#include "UHE/Debug/RenderDoc.h"
#include <cstdlib>
#include <filesystem>
#include <renderdoc_app.h>
#include "UHE/Core/CommandLine.h"
#include "UHE/Debug/GpuDebugOptions.h"
#if defined(_WIN32)
    #include <Windows.h>
#else
    #include <dlfcn.h>
#endif

namespace UHE::RenderDoc
{
namespace
{
RENDERDOC_API_1_6_0* s_Api = nullptr;
u64 s_CaptureFrame = 0; ///< -renderdoc-frame; 0 for none
u32 s_LoggedCaptures = 0;
std::string s_PendingNote; ///< written into the comments of the next capture
u64 s_FrameIndex = 0;

bool Requested()
{
    const char* env = std::getenv("UHE_RENDERDOC");
    const bool envOn = env != nullptr && *env != '\0' && std::string(env) != "0";
    return envOn || CommandLine::HasFlag("renderdoc") || !CommandLine::GetValue("renderdoc-frame").empty() ||
           GetGpuDebugOptions().RequestRenderDoc;
}

/// RenderDoc's GetAPI from the library already in the process, or loaded now when \p load.
pRENDERDOC_GetAPI FindGetApi(bool load)
{
#if defined(_WIN32)
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (module == nullptr && load)
        module = LoadLibraryA("renderdoc.dll");
    return module != nullptr ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"))
                             : nullptr;
#else
    void* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (module == nullptr && load)
        module = dlopen("librenderdoc.so", RTLD_NOW);
    return module != nullptr ? reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(module, "RENDERDOC_GetAPI")) : nullptr;
#endif
}
} // namespace

void Init()
{
    if (s_Api != nullptr)
        return;
    const bool requested = Requested();
    pRENDERDOC_GetAPI getApi = FindGetApi(requested);
    if (getApi == nullptr)
    {
        if (requested)
            UHE_CORE_WARN("RenderDoc: requested, but librenderdoc could not be loaded (is RenderDoc installed?)");
        return;
    }
    if (getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&s_Api)) != 1 || s_Api == nullptr)
    {
        s_Api = nullptr;
        UHE_CORE_WARN("RenderDoc: the loaded RenderDoc does not offer API 1.6");
        return;
    }
    std::string prefix = CommandLine::GetValue("renderdoc-out");
    if (prefix.empty())
        prefix = "captures/uhe";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(prefix).parent_path(), ec);
    s_Api->SetCaptureFilePathTemplate(prefix.c_str());
    // The engine draws its own overlay; RenderDoc's only covers the game.
    s_Api->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    // F12 captures in every app (the default also has PrintScreen, which
    // desktop screenshot tools take first).
    RENDERDOC_InputButton keys[] = {eRENDERDOC_Key_F12};
    s_Api->SetCaptureKeys(keys, 1);
    const std::string frame = CommandLine::GetValue("renderdoc-frame");
    if (!frame.empty())
        s_CaptureFrame = std::strtoull(frame.c_str(), nullptr, 10);
    int major = 0, minor = 0, patch = 0;
    s_Api->GetAPIVersion(&major, &minor, &patch);
    UHE_CORE_INFO("RenderDoc {0}.{1}.{2}: attached; captures go to {3}*", major, minor, patch, prefix);
}

bool Available()
{
    return s_Api != nullptr;
}

void TriggerCapture(const std::string& note)
{
    if (s_Api == nullptr)
        return;
    s_PendingNote = note;
    s_Api->TriggerCapture();
    UHE_CORE_INFO("RenderDoc: capturing the next frame");
}

void OnFrame(u64 frameIndex)
{
    if (s_Api == nullptr)
        return;
    s_FrameIndex = frameIndex;
    if (s_CaptureFrame != 0 && frameIndex == s_CaptureFrame)
        TriggerCapture("-renderdoc-frame " + std::to_string(frameIndex));
    // Written asynchronously: report each capture once it exists.
    const u32 count = s_Api->GetNumCaptures();
    for (; s_LoggedCaptures < count; ++s_LoggedCaptures)
    {
        char path[1024] = {};
        u32 length = sizeof(path);
        u64 timestamp = 0;
        if (s_Api->GetCapture(s_LoggedCaptures, path, &length, &timestamp) != 1)
            continue;
        // Annotated once written: RenderDoc shows the comments when it opens the file.
        std::string comments = "UHE, written around frame " + std::to_string(s_FrameIndex);
        if (!s_PendingNote.empty())
            comments += "\n" + s_PendingNote;
        s_Api->SetCaptureFileComments(path, comments.c_str());
        s_PendingNote.clear();
        UHE_CORE_INFO("RenderDoc: wrote {0}", path);
    }
}

u32 CaptureCount()
{
    return s_Api != nullptr ? s_Api->GetNumCaptures() : 0;
}

std::string LastCapturePath()
{
    const u32 count = CaptureCount();
    if (count == 0)
        return {};
    char path[1024] = {};
    u32 length = sizeof(path);
    u64 timestamp = 0;
    return s_Api->GetCapture(count - 1, path, &length, &timestamp) == 1 ? std::string(path) : std::string();
}

bool LaunchReplayUI()
{
    if (s_Api == nullptr)
        return false;
    // Connect the UI to this process; open the last capture if there is one.
    const std::string last = LastCapturePath();
    return s_Api->LaunchReplayUI(1, last.empty() ? nullptr : last.c_str()) != 0;
}

} // namespace UHE::RenderDoc
