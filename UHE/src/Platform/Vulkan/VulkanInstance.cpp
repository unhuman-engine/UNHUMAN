#define VK_NO_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include "uhepch.h"
#include "VulkanInstance.h"
#include <volk.h>
#include "Platform/Vulkan/ValidationGate.h"
#include "Platform/Vulkan/VulkanDebugUtils.h"

std::vector<const char*> validationLayers = {"VK_LAYER_KHRONOS_validation"};
#ifdef NDEBUG
constexpr bool enableValidationLayers = false;
#else
constexpr bool enableValidationLayers = true;
#endif

namespace UHE::RHI::VULKAN
{

namespace
{
// The gate's severity bits are a Vulkan-free mirror of the layer's, so its logic
// is testable without a Vulkan header. These keep the mirror honest: if either
// side is ever renumbered, the engine stops compiling instead of silently
// classifying an error as a warning.
static_assert(static_cast<unsigned>(vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose) ==
              static_cast<unsigned>(Validation::Verbose));
static_assert(static_cast<unsigned>(vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo) ==
              static_cast<unsigned>(Validation::Info));
static_assert(static_cast<unsigned>(vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning) ==
              static_cast<unsigned>(Validation::Warning));
static_assert(static_cast<unsigned>(vk::DebugUtilsMessageSeverityFlagBitsEXT::eError) ==
              static_cast<unsigned>(Validation::Error));
} // namespace

VulkanInstance::VulkanInstance() : m_context(), m_instance(nullptr), m_debugMessenger(nullptr)
{
    if (volkInitialize() != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to initialize Volk!");
    }
    if (!glfwVulkanSupported())
    {
        UHE_CORE_ERROR("Vulkan is not supported on this system!");
    }
}

void VulkanInstance::initialize()
{
    UHE_PROFILE_FUNCTION();
    vk::ApplicationInfo appInfo{
        .pApplicationName = "Unhuman Engine",
        .applicationVersion = VK_MAKE_VERSION(1, 3, 0),
        .pEngineName = "UHE Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3
    };

    std::vector<const char*> requiredLayer;
    if (enableValidationLayers)
    {
        requiredLayer.assign(validationLayers.begin(), validationLayers.end());
    }

    auto layerProperties = m_context.enumerateInstanceLayerProperties();
    if (std::ranges::any_of(requiredLayer,
                            [&layerProperties](auto const& requiredLayer)
                            {
                                return std::ranges::none_of(
                                    layerProperties, [requiredLayer](auto const& layerProperties)
                                    { return strcmp(layerProperties.layerName, requiredLayer) == 0; });
                            }))
    {
        throw std::runtime_error("Validation layers requested, but not available!");
    }

    auto requiredExtensions = getRequiredExtensions();

    auto requiredExtensionsProperties = m_context.enumerateInstanceExtensionProperties();
    if (std::ranges::any_of(requiredExtensions,
                            [&requiredExtensionsProperties](auto const& requiredExtension)
                            {
                                return std::ranges::none_of(
                                    requiredExtensionsProperties, [requiredExtension](auto const& extensionProperties)
                                    { return strcmp(extensionProperties.extensionName, requiredExtension) == 0; });
                            }))
    {
        throw std::runtime_error("Required extensions are not available!");
    };

    vk::DebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (enableValidationLayers)
    {
        debugCreateInfo.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
                                          vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                                          vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;

        debugCreateInfo.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                                      vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                                      vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
        debugCreateInfo.pfnUserCallback = debugCallback;
    }

    vk::InstanceCreateInfo createInfo{
        .pNext = enableValidationLayers ? &debugCreateInfo : nullptr,
        .flags = {},
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = static_cast<uint32_t>(requiredLayer.size()),
        .ppEnabledLayerNames = requiredLayer.data(),
        .enabledExtensionCount = static_cast<uint32_t>(requiredExtensions.size()),
        .ppEnabledExtensionNames = requiredExtensions.empty() ? nullptr : requiredExtensions.data()
    };

    m_instance = vk::raii::Instance(m_context, createInfo);

    volkLoadInstance(*m_instance);
    if (enableValidationLayers)
    {
        m_debugMessenger = vk::raii::DebugUtilsMessengerEXT(m_instance, debugCreateInfo);
        // Positive evidence that validation is on. A gate that only looks for the
        // absence of errors passes vacuously on a build without layers, and this
        // build has them off under NDEBUG -- so the run has to say so.
        UHE_CORE_INFO("[VALIDATION] enabled: layers on, errors stop the process = {}",
                      Validation::FatalValidationEnabled());
    }
    else
    {
        UHE_CORE_INFO("[VALIDATION] disabled: this build has no validation layers");
    }
}

std::vector<const char*> VulkanInstance::getRequiredExtensions()
{
    u32 glfwExtensionCount = 0;
    auto glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    // Names and labels for GPU debuggers whenever the driver has the
    // extension, not only with validation: a RenderDoc capture of a release
    // build shows each pass and object by name (DebugUtils).
    bool debugUtils = enableValidationLayers;
    if (!debugUtils)
    {
        for (const auto& property : m_context.enumerateInstanceExtensionProperties())
            debugUtils = debugUtils || strcmp(property.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
    }
    if (debugUtils)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    DebugUtils::SetEnabled(debugUtils);
    return extensions;
}

VKAPI_ATTR vk::Bool32 VKAPI_CALL
VulkanInstance::debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT severity, vk::DebugUtilsMessageTypeFlagsEXT type,
                              const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData, void*)
{
    // One filter for the process: the layer reports the same defect once per
    // frame, and only the first occurrence is worth reading.
    static Validation::RepeatFilter filter;

    const std::string key = Validation::MessageKey(pCallbackData->pMessageIdName, pCallbackData->messageIdNumber);
    const Validation::RepeatVerdict verdict = filter.Observe(key);
    if (verdict == Validation::RepeatVerdict::Suppressed)
    {
        return vk::False;
    }
    const std::string repeat = verdict == Validation::RepeatVerdict::Report
                                   ? " (repeated " + std::to_string(filter.Count(key)) + " times)"
                                   : std::string();

    switch (Validation::Classify(static_cast<unsigned>(severity)))
    {
        case Validation::Severity::Error:
            UHE_CORE_ERROR("Validation layer: {0}{1}", pCallbackData->pMessage, repeat);
            // A validation error is a defect, not a note to read past. CI sets
            // UHE_VALIDATION_FATAL so a run that trips one fails instead of
            // surviving to the end and looking green -- which is exactly how 34
            // per-frame VUIDs went unnoticed here.
            if (Validation::FatalValidationEnabled())
            {
                std::cerr << "UHE_VALIDATION_FATAL: validation error, stopping.\n";
                // `_Exit`, not `exit`: this runs inside a driver callback, and the
                // static destructors `exit` would run tear the device down from
                // there -- which hangs instead of returning, so a CI run would see
                // a timeout rather than the failure it is looking for.
                std::_Exit(1);
            }
            break;
        case Validation::Severity::Warning:
            // Warnings are not errors: the layer's "this write is unused" and
            // best-practice notes land here, and logging them at error severity
            // makes a correct frame look like a broken one.
            UHE_CORE_WARN("Validation layer: {0}{1}", pCallbackData->pMessage, repeat);
            break;
        default:
            UHE_CORE_INFO("Validation layer: {0}{1}", pCallbackData->pMessage, repeat);
            break;
    }
    return vk::False;
}

void VulkanInstance::cleanup()
{
    UHE_PROFILE_FUNCTION();
}
} // namespace UHE::RHI::VULKAN
