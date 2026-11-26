#include "uhepch.h"
#include "UHE/Debug/GpuDebugOptions.h"

namespace UHE
{

GpuDebugOptions& GetGpuDebugOptions()
{
    static GpuDebugOptions options;
    return options;
}

} // namespace UHE
