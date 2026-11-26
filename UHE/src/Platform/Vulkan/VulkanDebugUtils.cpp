#define VK_NO_PROTOTYPES
#include "uhepch.h"
#include "Platform/Vulkan/VulkanDebugUtils.h"
#include <volk.h>

namespace UHE::RHI::VULKAN::DebugUtils
{
namespace
{
bool s_Enabled = false;

/// A stable colour per name, so a pass keeps its colour across captures.
void ColourOf(const char* name, float out[4])
{
    std::uint32_t hash = 2166136261u;
    for (const char* c = name; *c != '\0'; ++c)
        hash = (hash ^ static_cast<unsigned char>(*c)) * 16777619u;
    out[0] = 0.35f + 0.6f * static_cast<float>(hash & 0xffu) / 255.0f;
    out[1] = 0.35f + 0.6f * static_cast<float>((hash >> 8) & 0xffu) / 255.0f;
    out[2] = 0.35f + 0.6f * static_cast<float>((hash >> 16) & 0xffu) / 255.0f;
    out[3] = 1.0f;
}
} // namespace

void SetEnabled(bool enabled)
{
    s_Enabled = enabled;
}

bool Enabled()
{
    return s_Enabled;
}

void Name(VkDevice device, int objectType, std::uint64_t handle, const char* name)
{
    // The entry point is loaded only when the extension was enabled.
    if (!s_Enabled || vkSetDebugUtilsObjectNameEXT == nullptr || device == nullptr || handle == 0 || name == nullptr ||
        *name == '\0')
        return;
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = static_cast<VkObjectType>(objectType);
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device, &info);
}

void BeginLabel(VkCommandBuffer cmd, const char* name)
{
    if (!s_Enabled || vkCmdBeginDebugUtilsLabelEXT == nullptr || cmd == nullptr)
        return;
    VkDebugUtilsLabelEXT label{};
    label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    label.pLabelName = name;
    ColourOf(name, label.color);
    vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
}

void EndLabel(VkCommandBuffer cmd)
{
    if (!s_Enabled || vkCmdEndDebugUtilsLabelEXT == nullptr || cmd == nullptr)
        return;
    vkCmdEndDebugUtilsLabelEXT(cmd);
}

} // namespace UHE::RHI::VULKAN::DebugUtils
