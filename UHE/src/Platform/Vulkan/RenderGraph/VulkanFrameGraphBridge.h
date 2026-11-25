#pragma once

#include <string>
#include <utility>
#include <vector>
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraph.h"
#include "UHE/RHI/FrameGraph/FrameGraph.h"
#include "UHE/RHI/FrameGraph/TransientPool.h"

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

    /**
     * @brief The object behind the texture \p frontend exports as \p name (RHIDevice::GetExportedTexture).
     *
     * Allocates it when this frame declares the export, so code that runs
     * before the frame is realized (the UI) gets the object the scene pass
     * will render; Translate later finds the same one. A name this frame does
     * not export gives the object a previous frame left, or nullptr.
     */
    [[nodiscard]] TextureHandle ExportedTexture(const FrameGraph& frontend, const std::string& name);

    /// @brief The translated frame ran: its exports are now in their final states.
    void CommitExports();

    /// @brief Ends the frame: pooled objects nothing used for a while are released.
    void EndFrame()
    {
        m_Pool.EndFrame();
        m_BufferPool.EndFrame();
        m_Exports.EndFrame();
        m_PendingExports.clear();
    }
    /// @brief Releases every pooled object. Call with the device idle.
    void Shutdown()
    {
        m_Pool.Clear();
        m_BufferPool.Clear();
        m_Exports.Clear();
        m_PendingExports.clear();
    }

    [[nodiscard]] const TransientTexturePool& Pool() const { return m_Pool; }
    [[nodiscard]] const TransientBufferPool& BufferPool() const { return m_BufferPool; }
    [[nodiscard]] const ExportPool& Exports() const { return m_Exports; }

private:
    VulkanDevice& m_Device;
    TransientTexturePool m_Pool;
    TransientBufferPool m_BufferPool;
    ExportPool m_Exports;
    // Exports this frame translated, with the state the frame leaves them in;
    // applied by CommitExports once the frame has run.
    std::vector<std::pair<std::string, FGState>> m_PendingExports;
    // This frame's mapping from frontend resource index to the physical object;
    // the execute callbacks read it while the render graph records.
    std::vector<TextureHandle> m_Textures;
    std::vector<BufferHandle> m_Buffers;
};

} // namespace UHE::RHI::VULKAN
