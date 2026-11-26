#pragma once
#include <memory>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/ImGui/ImGuiLayer.h"
struct ImGui_ImplVulkan_InitInfo;

namespace UHE::RHI::VULKAN
{
class VulkanDevice;

class VulkanImGuiLayer final : public ImGuiLayer
{
public:
    VulkanImGuiLayer(VulkanDevice* device);
    ~VulkanImGuiLayer() override = default;

    void OnAttach() override;
    void OnDetach() override;
    void Begin() override;
    void End() override;

private:
    /**
     * @brief Points \p info at ImGuiSrgb.slang when the swapchain is sRGB.
     *
     * ImGui's own shaders write its sRGB colours unchanged, and an sRGB
     * swapchain encodes them again: the whole UI came out washed-out grey.
     * Without the shader (a build that has not compiled it) ImGui keeps its
     * own and the run continues, with a warning.
     */
    void UseSrgbShaders(ImGui_ImplVulkan_InitInfo& info, VkFormat colorFormat);

    VulkanDevice* m_Device;
    std::unique_ptr<vk::raii::DescriptorPool> m_DescriptorPool;
    // SPIR-V of the replacement shaders: the backend keeps pointers to it and
    // creates its modules again when it rebuilds the pipeline.
    std::vector<u32> m_VertexSpirv;
    std::vector<u32> m_FragmentSpirv;
};
} // namespace UHE::RHI::VULKAN
