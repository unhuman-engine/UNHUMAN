#pragma once
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include "UHE/Core/Core.h"

namespace UHE::RHI::VULKAN
{

/// Sentinel stored in a buffer's bindless slot before it is registered.
inline constexpr u32 kInvalidBindlessIndex = static_cast<u32>(-1);

/**
 * \brief GPU buffer (vertex/index/uniform/storage) backed by Vulkan Memory Allocator.
 * \see https://docs.vulkan.org/refpages/latest/refpages/source/VkBuffer.html
 */
class VulkanBuffer
{
public:
    VulkanBuffer() = default;
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    void init(VmaAllocator allocator, vk::DeviceSize size, vk::BufferUsageFlags usage, VmaMemoryUsage memoryUsage);
    /// Writes \p size bytes into host-visible memory (buffer must be CPU-mappable).
    void UploadData(const void* data, vk::DeviceSize size, vk::DeviceSize offset = 0);
    /// Records a device-local copy of \p size bytes into \p dstBuffer.
    void CopyTo(VulkanBuffer& dstBuffer, vk::DeviceSize size, vk::raii::CommandBuffer& commandBuffer);
    void Destroy();

    [[nodiscard]] vk::Buffer GetHandle() const { return m_Buffer; }
    [[nodiscard]] vk::DeviceSize GetSize() const { return m_Size; }

    [[nodiscard]] u32 GetBindlessIndex() const { return m_BindlessIndex; }
    void SetBindlessIndex(u32 index) { m_BindlessIndex = index; }

    // Slot in the material-buffer array (global set, binding 2). A buffer only
    // ever lives in ONE of the two namespaces; the fields exist separately
    // because the two arrays are distinct SPIR-V bindings and their slot
    // spaces must not be mixed up.
    [[nodiscard]] u32 GetMaterialBindlessIndex() const { return m_MaterialBindlessIndex; }
    void SetMaterialBindlessIndex(u32 index) { m_MaterialBindlessIndex = index; }

private:
    u32 m_BindlessIndex = kInvalidBindlessIndex;
    u32 m_MaterialBindlessIndex = kInvalidBindlessIndex;
    VmaAllocator m_Allocator = nullptr;
    vk::Buffer m_Buffer = nullptr;
    VmaAllocation m_Allocation = nullptr;
    vk::DeviceSize m_Size = 0;
};

/// Vertex buffer: owns a GPU buffer sized to \c vertexCount * \c stride.
class VulkanVertexBuffer
{
public:
    VulkanVertexBuffer() = default;
    VulkanVertexBuffer(const VulkanVertexBuffer&) = delete;
    VulkanVertexBuffer& operator=(const VulkanVertexBuffer&) = delete;

    void Create(VmaAllocator allocator, const void* vertexData, uint32_t vertexCount, uint32_t stride);
    [[nodiscard]] vk::Buffer GetHandle() const { return m_Buffer.GetHandle(); }
    [[nodiscard]] u32 GetVertexCount() const { return m_VertexCount; }

private:
    VulkanBuffer m_Buffer;
    u32 m_VertexCount = 0;
};

/// Index buffer: owns a 32-bit index GPU buffer.
class VulkanIndexBuffer
{
public:
    VulkanIndexBuffer() = default;
    VulkanIndexBuffer(const VulkanIndexBuffer&) = delete;
    VulkanIndexBuffer& operator=(const VulkanIndexBuffer&) = delete;

    void Create(VmaAllocator allocator, const std::vector<uint32_t>& indices);
    [[nodiscard]] vk::Buffer GetHandle() const { return m_Buffer.GetHandle(); }
    [[nodiscard]] u32 GetIndexCount() const { return m_IndexCount; }

private:
    VulkanBuffer m_Buffer;
    u32 m_IndexCount = 0;
};

} // namespace UHE::RHI::VULKAN
