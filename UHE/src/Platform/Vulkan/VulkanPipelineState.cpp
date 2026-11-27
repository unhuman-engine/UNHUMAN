#include "VulkanPipelineState.h"
#include "VulkanShader.h"
#include <cstdint>
#include <type_traits>

namespace UHE::RHI::VULKAN
{
namespace
{
void HashAdd(u64& seed, u64 value)
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

void HashAdd(u64& seed, u32 value)
{
    HashAdd(seed, static_cast<u64>(value));
}

void HashAdd(u64& seed, bool value)
{
    HashAdd(seed, static_cast<u64>(value));
}

void HashAdd(u64& seed, const void* pointer)
{
    HashAdd(seed, static_cast<u64>(reinterpret_cast<uintptr_t>(pointer)));
}

template <typename T>
void HashAddEnum(u64& seed, T value)
{
    HashAdd(seed, static_cast<u64>(static_cast<std::underlying_type_t<T>>(value)));
}
} // namespace

u64 VulkanPipelineStateCache::Hash(const GraphicsPipelineDesc& desc)
{
    u64 seed = 0x243F6A8885A308D3ULL;

    const auto* vertexShader = reinterpret_cast<const VulkanShader*>(desc.vertexShader);
    const auto* fragmentShader = reinterpret_cast<const VulkanShader*>(desc.fragmentShader);
    HashAdd(seed, vertexShader ? vertexShader->GetStableId() : 0);
    HashAdd(seed, fragmentShader ? fragmentShader->GetStableId() : 0);

    const auto& elements = desc.vertexLayout.GetElements();
    HashAdd(seed, static_cast<u64>(elements.size()));
    for (const auto& element : elements)
    {
        HashAddEnum(seed, element.Type);
        HashAdd(seed, element.Size);
        HashAdd(seed, element.Offset);
        HashAdd(seed, element.Normalized);
    }
    HashAdd(seed, desc.vertexLayout.GetStride());

    HashAddEnum(seed, desc.topology);
    HashAddEnum(seed, desc.cullMode);
    HashAdd(seed, desc.colorAttachmentCount);
    for (u32 i = 0; i < desc.colorAttachmentCount; ++i)
        HashAddEnum(seed, desc.colorFormats[i]);
    HashAddEnum(seed, desc.depthFormat);
    HashAddEnum(seed, desc.blendMode);
    HashAdd(seed, desc.depthTest);
    HashAdd(seed, desc.depthWrite);
    HashAdd(seed, desc.wireframe);
    HashAdd(seed, desc.depthAttachmentOnly);
    HashAdd(seed, desc.pushConstantSize);

    const RenderPassDesc& renderPass = desc.renderPassDesc;
    HashAdd(seed, renderPass.colorAttachmentCount);
    for (u32 i = 0; i < renderPass.colorAttachmentCount; ++i)
    {
        const ColorAttachment& attachment = renderPass.colorAttachments[i];
        HashAddEnum(seed, attachment.loadOp);
        HashAddEnum(seed, attachment.storeOp);
        HashAddEnum(seed, attachment.format);
        HashAddEnum(seed, attachment.initialusage);
        HashAddEnum(seed, attachment.finalusage);
        HashAdd(seed, attachment.sampleCount);
        HashAddEnum(seed, attachment.stencilLoadOp);
        HashAddEnum(seed, attachment.stencilStoreOp);
    }

    HashAdd(seed, renderPass.subpassCount);
    for (u32 i = 0; i < renderPass.subpassCount; ++i)
    {
        const SubpassDesc& subpass = renderPass.subpasses[i];
        HashAdd(seed, subpass.colorAttachmentCount);
        for (u32 j = 0; j < subpass.colorAttachmentCount; ++j)
            HashAdd(seed, subpass.colorAttachments[j]);
        HashAdd(seed, subpass.depthAttachment);
    }

    HashAdd(seed, renderPass.dependencyCount);
    HashAdd(seed, renderPass.hasDepth);
    HashAddEnum(seed, renderPass.depthAttachment.loadOp);
    HashAddEnum(seed, renderPass.depthAttachment.storeOp);

    // Extra descriptor sets change the pipeline layout (issue #30).
    HashAdd(seed, desc.descriptorSetLayoutCount);
    for (u32 i = 0; i < desc.descriptorSetLayoutCount && i < desc.descriptorSetLayouts.size(); ++i)
        HashAdd(seed, reinterpret_cast<std::uintptr_t>(desc.descriptorSetLayouts[i]));

    return seed;
}

u64 VulkanPipelineStateCache::Hash(const ComputePipelineDesc& desc)
{
    u64 seed = 0x9E3779B97F4A7C15ULL;

    const auto* computeShader = reinterpret_cast<const VulkanShader*>(desc.computeShader);
    HashAdd(seed, computeShader ? computeShader->GetStableId() : 0);
    HashAdd(seed, desc.pushConstantSize);
    HashAdd(seed, desc.descriptorSetLayoutCount);
    for (u32 i = 0; i < desc.descriptorSetLayoutCount && i < desc.descriptorSetLayouts.size(); ++i)
        HashAdd(seed, reinterpret_cast<std::uintptr_t>(desc.descriptorSetLayouts[i]));

    return seed;
}

PipelineHandle VulkanPipelineStateCache::Acquire(const GraphicsPipelineDesc& desc, const CreateFn& create)
{
    const auto* vertexShader = reinterpret_cast<const VulkanShader*>(desc.vertexShader);
    const auto* fragmentShader = reinterpret_cast<const VulkanShader*>(desc.fragmentShader);
    const CacheKey key{Hash(desc), vertexShader ? vertexShader->GetStableId() : 0,
                       fragmentShader ? fragmentShader->GetStableId() : 0};
    return AcquireHashed(key, false, create);
}

PipelineHandle VulkanPipelineStateCache::Acquire(const ComputePipelineDesc& desc, const CreateFn& create)
{
    const auto* computeShader = reinterpret_cast<const VulkanShader*>(desc.computeShader);
    const CacheKey key{Hash(desc), computeShader ? computeShader->GetStableId() : 0, 0};
    return AcquireHashed(key, true, create);
}

PipelineHandle VulkanPipelineStateCache::AcquireHashed(const CacheKey& key, bool compute, const CreateFn& create)
{
    auto* map = compute ? &m_Compute : &m_Graphics;
    std::unique_lock<std::mutex> lock(m_Mutex);

    for (;;)
    {
        auto it = map->find(key);
        if (it == map->end())
        {
            (*map)[key].pending = true;

            lock.unlock();
            PipelineHandle handle = nullptr;
            try
            {
                handle = create();
            }
            catch (...)
            {
                lock.lock();
                map->erase(key);
                m_Cond.notify_all();
                throw;
            }
            lock.lock();

            auto ready = map->find(key);
            if (ready == map->end())
                ready = map->emplace(key, Entry{}).first;
            ready->second.handle = handle;
            ready->second.refCount = 1;
            ready->second.pending = false;
            m_Cond.notify_all();
            return handle;
        }

        if (it->second.pending)
        {
            m_Cond.wait(lock, [map, key]() {
                auto pending = map->find(key);
                return pending == map->end() || !pending->second.pending;
            });
            continue;
        }

        ++it->second.refCount;
        return it->second.handle;
    }
}

bool VulkanPipelineStateCache::Release(PipelineHandle handle)
{
    if (!handle)
        return false;

    std::unique_lock<std::mutex> lock(m_Mutex);

    const auto release = [&handle](auto& map) {
        for (auto it = map.begin(); it != map.end(); ++it)
        {
            if (it->second.handle != handle)
                continue;
            if (--it->second.refCount == 0)
            {
                map.erase(it);
                return true;
            }
            return false;
        }
        return false;
    };

    return release(m_Graphics) || release(m_Compute);
}
} // namespace UHE::RHI::VULKAN
