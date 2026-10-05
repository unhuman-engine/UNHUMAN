#pragma once
#include <GLFW/glfw3.h>
#include <vulkan/vulkan_raii.hpp>

namespace UHE::RHI::VULKAN
{
/**
 * \brief Owns the VkInstance and the debug-utils messenger.
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkInstance.html
 */
class VulkanInstance
{
public:
    VulkanInstance();
    VulkanInstance(const VulkanInstance&) = delete;
    VulkanInstance& operator=(const VulkanInstance&) = delete;

    void initialize();

    /// \returns the instance extensions required for the current platform (surface + debug).
    [[nodiscard]] std::vector<const char*> getRequiredExtensions();

    /// Validation-layer callback; routes Vulkan diagnostics into the engine log.
    static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
                                                          vk::DebugUtilsMessageTypeFlagsEXT type,
                                                          const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                          void*);

    void cleanup();
    [[nodiscard]] vk::raii::Instance& getInstance() { return m_instance; }

private:
    vk::raii::Context m_context;
    vk::raii::Instance m_instance = nullptr;
    vk::raii::DebugUtilsMessengerEXT m_debugMessenger = nullptr;
};
} // namespace UHE::RHI::VULKAN
