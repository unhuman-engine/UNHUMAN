#pragma once

#include <cstdint>
#include "UHE/Core/Core.h"

// Plain Vulkan handle types without pulling vulkan.h into every includer.
typedef struct VkDevice_T* VkDevice;
typedef struct VkCommandBuffer_T* VkCommandBuffer;

namespace UHE::RHI::VULKAN::DebugUtils
{

/**
 * @brief Names and labels for GPU debuggers (RenderDoc, Nsight, the validation layer).
 *
 * VK_EXT_debug_utils is enabled on the instance whenever the driver offers
 * it, not only with validation, so a capture of a release build still shows
 * each frame graph pass as a labelled region and each image, buffer and
 * pipeline by name. Without the extension every call here does nothing.
 */
void SetEnabled(bool enabled);
[[nodiscard]] bool Enabled();

/// @brief Names a Vulkan object: \p objectType is a VkObjectType, \p handle the object as an integer.
void Name(VkDevice device, int objectType, std::uint64_t handle, const char* name);

/// @brief Opens a labelled region in \p cmd (one per frame graph pass); close it with EndLabel.
void BeginLabel(VkCommandBuffer cmd, const char* name);
void EndLabel(VkCommandBuffer cmd);

} // namespace UHE::RHI::VULKAN::DebugUtils
