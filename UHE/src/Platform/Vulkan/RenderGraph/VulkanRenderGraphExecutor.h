#pragma once
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan_raii.hpp>
#include "Platform/Vulkan/VulkanBarrierEncoder.h"
#include "Platform/Vulkan/VulkanSemaphore.h"
#include "UHE/Core/Core.h"
#include "UHE/Jobsystem/Taskgraph.h"
#include "UHE/RHI/RHICore.h"
#include "UHE/RHI/RenderGraph/RenderGraphCompiler.h"
#include "UHE/RHI/RenderGraph/RenderGraphResources.h"
#include "UHE/RHI/RenderGraph/RenderGraphTypes.h"

// Render graph executor (docs/architecture/rendergraph.md §8.5) — step 3 slice.
//
// Turns the compiler's pure-data RGCompiledFrame into recorded + submitted GPU
// work, in three pieces:
//
//   Resolve() — binds RG handles to live Vulkan objects. The owner (the device,
//               once the frame loop is migrated, §14 step 4) registers the
//               swapchain images and engine textures/buffers per frame; Resolve
//               then fills complete VulkanBarrierEncoder-shaped barriers,
//               rendering scopes and a RGPassContext view. Pure CPU — no
//               command buffer touched, so it is unit-testable headlessly.
//   Record()  — encodes pre-barriers via VulkanBarrierEncoder (tier-aware,
//               Sync2 vs Legacy), opens a dynamic-rendering scope around
//               graphics passes (load/store/clear from the pass declaration —
//               the executor owns scopes; pass code never calls Begin/End),
//               invokes the pass execute fn via RGPassContext, encodes
//               post-barriers (finalState exits).
//   Submit()  — VulkanSemaphore::Submit wrapper (waits/signals/fence chosen by
//               the frame loop; v1 passes through).
//
// Step-3 boundaries (later milestones, not silently missing):
//   - Acquire/present edges and the Begin/End frame-loop move: §14 step 4.
//   - Legacy-tier render passes (cached VkRenderPass/Framebuffer, §8.8): this
//     slice requires dynamic rendering; the device enables KHR_dynamic_rendering.
//   - TaskGraph mapping and secondary recording: §14 step 5 / §8.5 opt-in.
//
// RGPassContext exposes NO barrier API (§8.4) — synchronization is the
// compiler's contract; the pass only consumes resolved views/buffers.

namespace UHE::RHI::VULKAN
{

class VulkanBarrierEncoder;
class VulkanCommandBuffer;

// ─── Resolved output (Vulkan handles filled; engine enums preserved) ────────

struct RGResolvedImageBarrier
{
    ImageBarrier barrier; // vk::Image + subresource range filled
};

struct RGResolvedBufferBarrier
{
    BufferBarrier barrier; // vk::Buffer filled
};

struct RGResolvedColorAttachment
{
    vk::ImageView view = nullptr;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    vk::ClearColorValue clearColor{std::array<float, 4>{0.f, 0.f, 0.f, 1.f}};
    TextureFormat format = TextureFormat::RGBA8_UNORM; ///< what a secondary list inherits
};

struct RGResolvedDepthAttachment
{
    vk::ImageView view = nullptr;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    vk::ClearDepthStencilValue clear{1.0f, 0u};
    TextureFormat format = TextureFormat::D32_FLOAT;
};

struct RGResolvedPass
{
    u32 compiledId = 0;
    u32 passIndex = 0;
    std::string name;
    RGPassType type = RGPassType::Graphics;
    std::vector<u32> dependencies; // compiled ids this pass depends on (DAG)
    // Graphics passes only; nullopt for compute/transfer (raw recording).
    std::optional<vk::Extent2D> renderExtent;
    std::vector<RGResolvedColorAttachment> colorAttachments;
    std::optional<RGResolvedDepthAttachment> depthAttachment;
    std::vector<RGResolvedImageBarrier> preImageBarriers;
    std::vector<RGResolvedImageBarrier> postImageBarriers;
    std::vector<RGResolvedBufferBarrier> preBufferBarriers;
};

struct RGResolvedFrame
{
    u64 topologyHash = 0;
    std::vector<RGResolvedPass> passes;
};

// ─── Pass callback surface (§8.4 slice) ─────────────────────────────────────
// Views/buffers resolve with correct state guaranteed by the compiler's
// pre-barriers; the command buffer is the frame's primary.

} // namespace UHE::RHI::VULKAN

namespace UHE::RHI
{
namespace VULKAN
{
class VulkanRenderGraphExecutor;
}

/// The pass context the graph (UHE/RHI/RenderGraph) declares and hands each
/// pass: an opaque type to the graph, defined by the backend that records it.
class UHE_API RGPassContext
{
public:
    RGPassContext(const VULKAN::VulkanRenderGraphExecutor& executor, vk::CommandBuffer cmd, u32 passIndex,
                  vk::Extent2D extent, RHICommandBuffer* list = nullptr)
        : m_Executor(executor), m_Cmd(cmd), m_PassIndex(passIndex), m_Extent(extent), m_List(list)
    {
    }

    /// The pass's own command list when the graph records one per pass
    /// (VulkanRenderGraphExecutor::MapToTaskgraphPerPass); null when it records
    /// into the frame's primary. The executor then times and labels the pass
    /// itself, from the primary, around the list.
    [[nodiscard]] RHICommandBuffer* List() const { return m_List; }

    // Resolved, correct-state view of a texture (shader-read, storage, etc.).
    [[nodiscard]] vk::ImageView View(RGTextureHandle texture) const;
    // Resolved buffer device address carrier.
    [[nodiscard]] vk::Buffer Buffer(RGBufferHandle buffer) const;
    // Extent of this pass's render area (first attachment; 0 for scope-less).
    [[nodiscard]] vk::Extent2D Extent() const { return m_Extent; }
    // Raw command buffer for recording (dispatch, draws, pushes).
    [[nodiscard]] vk::CommandBuffer Cmd() const { return m_Cmd; }
    // Owning binary's dispatcher for raw-handle calls. Out-of-line so it
    // binds the engine's initialized copy across the DLL boundary.
    [[nodiscard]] const vk::detail::DispatchLoaderDynamic& Dispatcher() const;

    [[nodiscard]] u32 PassIndex() const { return m_PassIndex; }

private:
    const VULKAN::VulkanRenderGraphExecutor& m_Executor;
    vk::CommandBuffer m_Cmd;
    u32 m_PassIndex = 0;
    vk::Extent2D m_Extent{};
    RHICommandBuffer* m_List = nullptr;
};

} // namespace UHE::RHI

namespace UHE::RHI::VULKAN
{

class VulkanRenderGraphExecutor
{
public:
    /// A pass's own command list: the RHI list and its Vulkan handle.
    struct PassList
    {
        RHICommandBuffer* List = nullptr;
        vk::CommandBuffer Handle = nullptr;
    };
    /// Gives a pass a secondary list to record into (CommandListDesc says how).
    using PassListAllocator = std::function<PassList(const CommandListDesc&)>;
    /// Makes \p list the one this thread's renderers record into (null: none).
    using ThreadListBinder = std::function<void(RHICommandBuffer* list)>;
    /// Called on the primary around a pass's list: GPU timing and debug labels.
    using PassHook = std::function<void(const RGResolvedPass&)>;

    VulkanRenderGraphExecutor() = default;
    VulkanRenderGraphExecutor(const VulkanRenderGraphExecutor&) = delete;
    VulkanRenderGraphExecutor& operator=(const VulkanRenderGraphExecutor&) = delete;
    ~VulkanRenderGraphExecutor() = default;

    // ─── Per-frame resource registration (owner: device, §14 step 4) ────────
    // Both the backing image (barriers) and the view (attachments, RGPassContext)
    // are required; extent comes from the live object, not the declaration.
    // Format drives attachment clear-value typing (R32_SINT entity targets
    // clear as ints — mirrors the legacy BeginRenderPass special case).
    void RegisterTexture(RGTextureHandle texture, vk::Image image, vk::ImageView view,
                         u32 width, u32 height, TextureFormat format);
    void RegisterBuffer(RGBufferHandle buffer, vk::Buffer vkBuffer);
    void ClearRegistrations();

    // ─── Resolve ────────────────────────────────────────────────────────────
    // Binds the compiled frame to registered Vulkan objects. Missing
    // registrations are collected in `errors` (named handle + pass) while the
    // rest keeps resolving; Record must not be called until `errors` is empty.
    [[nodiscard]] RGResolvedFrame Resolve(const RGCompiledFrame& compiled,
                                          const std::vector<RGPassSpec>& specs,
                                          std::vector<std::string>& errors) const;

    // ─── Record ─────────────────────────────────────────────────────────────
    // Encodes barriers + rendering scopes + pass callbacks into `target`.
    // `encoder` decides Sync2 vs Legacy encoding. Missing registrations were
    // already reported by Resolve; Record skips unresolved uses defensively.
    void Record(const RGResolvedFrame& resolved, const std::vector<RGPassSpec>& specs,
                VulkanCommandBuffer& target, VulkanBarrierEncoder& encoder,
                std::vector<std::string>& errors) const;

    // Range slice of Record: [firstSlot, endSlot) into resolved.passes — the
    // unit one TaskGraph node records (§8.5). Recorded in slot order.

    // ─── Submit ─────────────────────────────────────────────────────────────
    // Queue submission through the tier-aware semaphore path. Waits/signals and
    // the fence belong to the frame loop (§14 step 4 wires acquire/present).
    static void Submit(VulkanContext* context, vk::raii::Queue& queue, vk::CommandBuffer cmd,
                       std::span<const SemaphoreWait> waits,
                       std::span<const SemaphoreSignal> signals, vk::Fence fence);

    // ─── TaskGraph mapping (§8.5, §14 step 5) ───────────────────────────────
    // One node per compiled pass; TaskGraph edges mirror the compiler's DAG
    // (dependencies → dependents). The device calls MapToTaskgraph during
    // Build, then ExecuteGraph once per frame after the per-pass submits.
    // Nodes record pass i+1 while pass i submits (pipelining; §8.5). Today all
    // recording lands on the frame's primary buffer, so TaskGraph execution
    // order is constrained to the topological order — verified by
    // tests/rendergraph (single-queue frame compiles to declaration order).
    // Takes the RAW command buffer: nodes record into it directly (headless
    // tests map without a VulkanCommandBuffer wrapper).
    // Returns the per-pass node ids (compiled order) for debug overlays.
    std::vector<Jobsystem::TaskID> MapToTaskgraph(Jobsystem::TaskGraph& graph,
                                                  const RGResolvedFrame& resolved,
                                                  const std::vector<RGPassSpec>& specs,
                                                  vk::CommandBuffer targetCmd,
                                                  VulkanBarrierEncoder& encoder) const;
    // Enqueues the mapped graph: callers run their per-pass submissions first,
    // then call this; the main thread helps execute while waiting (§8.5).
    void ExecuteGraph(Jobsystem::TaskGraph& graph, Jobsystem::UheJobsystem& jobs,
                      std::span<const Jobsystem::TaskID> nodes);

    // ─── Per-pass command lists (issue #32) ─────────────────────────────────
    // Each pass's body records into a secondary list of its own, on a job
    // thread; the primary keeps the barriers and rendering scopes and runs the
    // lists in compiled order (AssemblePerPass, main thread). Passes record
    // one after another unless RGPassSpec::parallelRecording says the pass
    // touches no state another pass does: renderers keep global state.

    /// @brief Builds the recording nodes; \p allocate hands each a secondary list.
    std::vector<Jobsystem::TaskID> MapToTaskgraphPerPass(Jobsystem::TaskGraph& graph, const RGResolvedFrame& resolved,
                                                         const std::vector<RGPassSpec>& specs,
                                                         const PassListAllocator& allocate,
                                                         const ThreadListBinder& bindThread) const;
    /// @brief Records barriers and scopes into \p primary around each pass's list, in compiled order.
    void AssemblePerPass(const RGResolvedFrame& resolved, const std::vector<RGPassSpec>& specs,
                         vk::CommandBuffer primary, VulkanBarrierEncoder& encoder, const PassHook& beginPass,
                         const PassHook& endPass) const;

    // ─── RGPassContext resolution (called from the callbacks) ───────────────
    [[nodiscard]] vk::ImageView ResolveView(RGTextureHandle texture) const;
    [[nodiscard]] vk::Buffer ResolveBuffer(RGBufferHandle buffer) const;
    [[nodiscard]] vk::Extent2D ResolveExtent(RGTextureHandle texture) const;

    [[nodiscard]] const std::vector<std::string>& RegistrationWarnings() const
    {
        return m_RegistrationWarnings;
    }

private:
    // TaskGraph node context (§8.5): POD view over one resolved pass, stable
    // address stored in m_NodeContexts so the job fn needs no capture.
    struct NodeContext
    {
        const VulkanRenderGraphExecutor* executor;
        const RGResolvedFrame* resolved;
        const std::vector<RGPassSpec>* specs;
        vk::CommandBuffer targetCmd; // raw handle — recording target
        VulkanBarrierEncoder* encoder;
        u32 passSlot; // index into resolved->passes
    };

    void RecordRange(const RGResolvedFrame& resolved, const std::vector<RGPassSpec>& specs,
                     u32 firstSlot, u32 endSlot, vk::CommandBuffer cmd,
                     VulkanBarrierEncoder& encoder, std::vector<std::string>& errors) const;

    struct PerPassNode
    {
        const VulkanRenderGraphExecutor* executor;
        const RGResolvedFrame* resolved;
        const std::vector<RGPassSpec>* specs;
        const PassListAllocator* allocate;
        const ThreadListBinder* bindThread;
        u32 passSlot;
    };
    mutable std::deque<PerPassNode> m_PerPassNodes;
    mutable std::vector<PassList> m_PassLists; ///< per slot; empty for a pass with no body

    struct RGRegisteredTexture
    {
        vk::Image image = nullptr;
        vk::ImageView view = nullptr;
        u32 width = 1;
        u32 height = 1;
        TextureFormat format = TextureFormat::RGBA8_UNORM;
        u32 generation = 0;
    };
    struct RGRegisteredBuffer
    {
        vk::Buffer buffer = nullptr;
        u32 generation = 0;
    };
    std::unordered_map<u32, RGRegisteredTexture> m_Textures; // registry slot → live object
    std::unordered_map<u32, RGRegisteredBuffer> m_Buffers;
    std::vector<std::string> m_RegistrationWarnings;

    // TaskGraph node contexts (stable addresses across node creation; §8.5).
    // Scratch, hence mutable: filled by the const MapToTaskgraph, cleared by
    // ExecuteGraph after the graph completes.
    mutable std::deque<NodeContext> m_NodeContexts;
};

} // namespace UHE::RHI::VULKAN
