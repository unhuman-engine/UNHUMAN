#pragma once

#include <string>
#include <vector>
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraph.h"
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/RHI/FrameGraph/TransientTexturePool.h"

namespace UHE::RHI::VULKAN
{

class VulkanDevice;

/**
 * @brief Realizes a FrameGraph frontend frame on the Vulkan render graph.
 *
 * Each frame the frontend's lowered frame becomes render graph declarations:
 * imports stay imports, created textures get a pooled physical texture, usages
 * become reads, writes and attachments, and each pass's execute callback runs
 * the frontend's with an FGContext that hands out an RHICommandBuffer. The
 * render graph then orders, culls and barriers the frame as it does for any
 * pass; nothing here tracks dependencies.
 */
class VulkanFrameGraphBridge
{
public:
    explicit VulkanFrameGraphBridge(VulkanDevice& device);

    /**
     * @brief Declares \p frontend's passes into \p graph.
     * @return false when the frame cannot be realized; \p errors says why and no pass was declared.
     */
    bool Translate(const FrameGraph& frontend, VulkanRenderGraph& graph, std::vector<std::string>& errors);

    /// @brief Ends the frame: pooled textures nothing used for a while are released.
    void EndFrame() { m_Pool.EndFrame(); }
    /// @brief Releases every pooled texture. Call with the device idle.
    void Shutdown() { m_Pool.Clear(); }

    [[nodiscard]] const TransientTexturePool& Pool() const { return m_Pool; }

private:
    VulkanDevice& m_Device;
    TransientTexturePool m_Pool;
    // This frame's mapping from frontend resource index to the physical object;
    // the execute callbacks read it while the render graph records.
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
};

} // namespace UHE::RHI::VULKAN
