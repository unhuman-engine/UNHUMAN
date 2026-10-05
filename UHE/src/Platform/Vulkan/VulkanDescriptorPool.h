#pragma once
/**
 * \file VulkanDescriptorPool.h
 * \brief Thin owner of a VkDescriptorPool (regular and bindless variants).
 *
 * \note Regular pools back a single set whose layout changes rarely; the bindless
 *       pool is sized once for the maximum resource count and reused forever.
 * \see  https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorPool.html
 */
#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

namespace UHE::RHI::VULKAN
{
class VulkanContext;

class VulkanDescriptorPool
{
public:
    VulkanDescriptorPool() = default;
    ~VulkanDescriptorPool() = default;
    VulkanDescriptorPool(const VulkanDescriptorPool&) = delete;
    VulkanDescriptorPool& operator=(const VulkanDescriptorPool&) = delete;

    void Init(VulkanContext* context);
    void CreateDescriptorPool();
    void CreateBindlessDescriptorPool(uint32_t maxBindlessResources);
    void DestroyDescriptorPool();
    void ResetDescriptorPool();
    void Allocate();

    void SetDescriptorPool(vk::raii::DescriptorPool& pool) { m_DescriptorPool = std::move(pool); }
    [[nodiscard]] const vk::raii::DescriptorPool& GetDescriptorPool() const { return m_DescriptorPool; }
    [[nodiscard]] vk::raii::DescriptorPool& GetDescriptorPool() { return m_DescriptorPool; }

private:
    static constexpr uint32_t MAX_BINDLESS_RESOURCES = 10000;
    VulkanContext* m_Context = nullptr;
    vk::raii::DescriptorPool m_DescriptorPool = nullptr;
};
} // namespace UHE::RHI::VULKAN
