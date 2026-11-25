#pragma once
#include <string>
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphBuilder.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphCompileCache.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphCompiler.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphResources.h"
#include "Platform/Vulkan/RenderGraph/VulkanRenderGraphTypes.h"
#include "UHE/Core/Core.h"


// Render graph facade (docs/architecture/rendergraph.md §8.1) — step 2 slice.
//
// Declare + validate + compile + dump. Execution lands with the executor
// milestone (§13.2 item 8), so nothing here touches the GPU yet.

namespace UHE::RHI::VULKAN
{

class UHE_API VulkanRenderGraph
{
public:
    VulkanRenderGraph() = default;
    VulkanRenderGraph(const VulkanRenderGraph&) = delete;
    VulkanRenderGraph& operator=(const VulkanRenderGraph&) = delete;
    ~VulkanRenderGraph() = default;

    // ─── Declaration (per frame, or persistent until Reset) ───

    RGTextureHandle CreateTexture(const RGTextureDesc& desc) { return m_Builder.CreateTexture(desc); }
    RGBufferHandle CreateBuffer(const RGBufferDesc& desc) { return m_Builder.CreateBuffer(desc); }

    // Swapchain-shaped import: identity hash separates generations of the
    // external resource (§9.1.4 — resize rebuilds the cache because this hash
    // enters the topology hash).
    RGTextureHandle ImportTexture(const std::string& name, u64 identityHash, u32 width, u32 height,
                                  u32 sampleCount, ImageState initialState, ImageState finalState)
    {
        const RGImportedDesc identity{identityHash, width, height, 1u, sampleCount};
        return m_Builder.ImportTexture(name, identity, initialState, finalState);
    }

    RGBufferHandle ImportBuffer(const std::string& name, u64 identityHash)
    {
        const RGImportedDesc identity{identityHash, 1u, 1u, 1u, 1u};
        return m_Builder.ImportBuffer(name, identity);
    }

    // Returns a reference into the builder's deque — valid until Reset(), so
    // both `auto& pass = rg.AddPass(...)` and full chaining work (§8.1).
    RGPassBuilder& AddPass(const std::string& name, RGPassType type = RGPassType::Graphics,
                           RGQueue queue = RGQueue::Graphics)
    {
        return m_Builder.AddPass(name, type, queue);
    }

    // ─── Finalization ───

    // §7 rules, collected (never throws). Empty result = the graph is valid.
    [[nodiscard]] std::vector<RGValidationError> Validate() const { return m_Builder.Validate(); }

    // §8.3 compiler (step 2): validate → topo sort → cull → lifetime → state
    // tracking + barrier synthesis → CompiledFrame. Pure data out — no GPU
    // work; the executor (step 3) consumes it. Never throws; errors collected.
    [[nodiscard]] RGCompileResult Compile() const
    {
        RGCompileResult result;
        result.errors = m_Builder.Validate();
        RGCompileResult compilerResult =
            VulkanRenderGraphCompiler::Compile(m_Builder.Passes(), m_Builder.Resources());
        result.errors.insert(result.errors.end(), compilerResult.errors.begin(), compilerResult.errors.end());
        result.frame = std::move(compilerResult.frame);
        // The compiler hashes the resource slice; the cache key is the full
        // resources + pass-structure hash from the builder.
        result.frame.topologyHash = m_Builder.ComputeTopologyHash();
        return result;
    }

    /// @brief Compile(), reusing the previous frame's result when the declared
    /// frame is structurally identical (see VulkanRenderGraphCompileCache).
    [[nodiscard]] RGCompileResult CompileCached() { return m_CompileCache.Compile(m_Builder); }
    [[nodiscard]] const VulkanRenderGraphCompileCache& CompileCache() const { return m_CompileCache; }

    // §8.3 cache key (resource material + pass structure slice for step 1).
    [[nodiscard]] u64 ComputeTopologyHash() const { return m_Builder.ComputeTopologyHash(); }

    // Debug/QA surface: pass to a logger, an ImGui panel, or a --dump-graph
    // file. Contains everything declared this frame, including errors.
    [[nodiscard]] RGRawGraph DumpRawGraph() const;
    [[nodiscard]] std::string DumpRawGraphJson() const;

    // ─── Frame reset ───
    // Bumps all generations: handles from the previous frame fail lookups
    // deterministically (stale-handle validation) instead of aliasing.
    void Reset() { m_Builder.Reset(); }

    [[nodiscard]] const VulkanRenderGraphBuilder& GetBuilder() const { return m_Builder; }

private:
    VulkanRenderGraphBuilder m_Builder;
    VulkanRenderGraphCompileCache m_CompileCache; // survives Reset(): it is keyed, not owned by a frame
};

} // namespace UHE::RHI::VULKAN
