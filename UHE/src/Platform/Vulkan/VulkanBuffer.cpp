#include "uhepch.h" // IWYU pragma: keep
#include "VulkanBuffer.h"
#include "UHE/Core/Log.h"

namespace UHE::RHI::VULKAN
{

VulkanBuffer::~VulkanBuffer()
{
    Destroy();
}

void VulkanBuffer::Destroy()
{
    if (m_Buffer && m_Allocator)
    {
        vmaDestroyBuffer(m_Allocator, static_cast<VkBuffer>(m_Buffer), m_Allocation);
        m_Buffer = nullptr;
        m_Allocation = nullptr;
    }
}

void VulkanBuffer::init(VmaAllocator allocator, vk::DeviceSize size, vk::BufferUsageFlags usage,
                        VmaMemoryUsage memoryUsage)
{
    m_Allocator = allocator;
    m_Size = size;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = static_cast<VkBufferUsageFlags>(usage);
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = memoryUsage;

    if (memoryUsage == VMA_MEMORY_USAGE_CPU_ONLY || memoryUsage == VMA_MEMORY_USAGE_CPU_TO_GPU)
    {
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }

    VkBuffer vkBuffer;
    VkResult res = vmaCreateBuffer(m_Allocator, &bufferInfo, &allocInfo, &vkBuffer, &m_Allocation, nullptr);
    UHE_CORE_ASSERT(res == VK_SUCCESS, "Failed to create Vulkan Buffer with VMA!");

    m_Buffer = vk::Buffer(vkBuffer);
}

void VulkanBuffer::UploadData(const void* data, vk::DeviceSize size, vk::DeviceSize offset)
{
    if (offset > m_Size || size > m_Size - offset)
    {
        UHE_CORE_ERROR("Upload size and offset exceed buffer size!");
        return;
    }
    if (size == 0)
        return;
    if (data == nullptr)
    {
        UHE_CORE_ERROR("Cannot upload null data to a Vulkan buffer!");
        return;
    }

    void* mappedData = nullptr;
    VkResult result = vmaMapMemory(m_Allocator, m_Allocation, &mappedData);
    if (result != VK_SUCCESS || mappedData == nullptr)
    {
        UHE_CORE_ERROR("Failed to map Vulkan buffer memory (VkResult={})", static_cast<i32>(result));
        return;
    }

    memcpy(static_cast<uint8_t*>(mappedData) + offset, data, size);
    VkResult flushResult = vmaFlushAllocation(m_Allocator, m_Allocation, offset, size);
    vmaUnmapMemory(m_Allocator, m_Allocation);
    if (flushResult != VK_SUCCESS)
        UHE_CORE_ERROR("Failed to flush Vulkan buffer memory (VkResult={})", static_cast<i32>(flushResult));
}

bool VulkanBuffer::ReadData(void* out, vk::DeviceSize size, vk::DeviceSize offset) const
{
    if (out == nullptr || offset + size > m_Size)
        return false;
    void* mappedData = nullptr;
    if (vmaMapMemory(m_Allocator, m_Allocation, &mappedData) != VK_SUCCESS || mappedData == nullptr)
        return false;
    // The GPU wrote it: make its writes visible to the host before reading.
    vmaInvalidateAllocation(m_Allocator, m_Allocation, offset, size);
    memcpy(out, static_cast<const uint8_t*>(mappedData) + offset, size);
    vmaUnmapMemory(m_Allocator, m_Allocation);
    return true;
}

void VulkanBuffer::CopyTo(VulkanBuffer& dstBuffer, vk::DeviceSize size, vk::raii::CommandBuffer& commandBuffer)
{
    vk::BufferCopy copyRegion{};
    copyRegion.srcOffset = 0;
    copyRegion.dstOffset = 0;
    copyRegion.size = size;

    commandBuffer.copyBuffer(m_Buffer, dstBuffer.GetHandle(), copyRegion);
}

// ------------------ Vertex Buffer ---------------

void VulkanVertexBuffer::Create(VmaAllocator allocator, const void* vertexData, uint32_t vertexCount, uint32_t stride)
{
    m_VertexCount = vertexCount;
    auto bufferSize = static_cast<vk::DeviceSize>(static_cast<size_t>(vertexCount) * stride);

    VulkanBuffer stagingBuffer;
    stagingBuffer.init(allocator, bufferSize, vk::BufferUsageFlagBits::eTransferSrc, VMA_MEMORY_USAGE_CPU_ONLY);
    stagingBuffer.UploadData(vertexData, bufferSize);

    m_Buffer.init(allocator, bufferSize, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer,
                  VMA_MEMORY_USAGE_GPU_ONLY);

    UHE_CORE_WARN("VulkanVertexBuffer staging copy command submission needs to be "
                  "handled by an immediate context!");
}

// ------------- Index Buffer ----------------

void VulkanIndexBuffer::Create(VmaAllocator allocator, const std::vector<uint32_t>& indices)
{
    m_IndexCount = static_cast<uint32_t>(indices.size());
    auto bufferSize = static_cast<vk::DeviceSize>(sizeof(indices[0]) * indices.size());

    VulkanBuffer stagingBuffer;
    stagingBuffer.init(allocator, bufferSize, vk::BufferUsageFlagBits::eTransferSrc, VMA_MEMORY_USAGE_CPU_ONLY);
    stagingBuffer.UploadData(indices.data(), bufferSize);

    m_Buffer.init(allocator, bufferSize, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer,
                  VMA_MEMORY_USAGE_GPU_ONLY);

    UHE_CORE_WARN("VulkanIndexBuffer staging copy command submission needs to be "
                  "handled by an immediate context!");
}

} // namespace UHE::RHI::VULKAN
