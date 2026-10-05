#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/RHI/RHITypes.h"

namespace UHE::RHI::VULKAN
{

/**
 * \brief Owns a VkShaderModule compiled from a SPIR-V blob.
 *
 * Every module gets a process-unique stable id, so pipeline caching can key on the
 * id instead of hashing the SPIR-V again.
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkShaderModule.html
 */
class VulkanShader
{
public:
    VulkanShader() = default;
    VulkanShader(const VulkanShader&) = delete;
    VulkanShader& operator=(const VulkanShader&) = delete;

    /// Creates the module from \p desc.spirvData.
    void Create(const vk::raii::Device& device, const ShaderDesc& desc);

    [[nodiscard]] vk::ShaderModule GetHandle() const { return *m_Module; }
    [[nodiscard]] ShaderStage GetStage() const { return m_Stage; }
    [[nodiscard]] const char* GetEntryPoint() const { return m_EntryPoint.c_str(); }
    [[nodiscard]] vk::ShaderModule GetModule() const { return *m_Module; }
    [[nodiscard]] u64 GetStableId() const { return m_StableId; }

private:
    vk::raii::ShaderModule m_Module{nullptr};
    ShaderStage m_Stage = ShaderStage::Vertex;
    std::string m_EntryPoint = "main";
    u64 m_StableId = s_NextId.fetch_add(1, std::memory_order_relaxed);
    inline static std::atomic<u64> s_NextId{1};
};

} // namespace UHE::RHI::VULKAN
