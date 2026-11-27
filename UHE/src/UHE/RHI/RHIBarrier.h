#pragma once
/**
 * @file RHIBarrier.h
 * @brief The engine's synchronisation vocabulary: pipeline stages, memory accesses, image states.
 *
 * Engine-level intent for barriers and submits (issue #30, RHI core). The render
 * graph compiler (UHE/RHI/RenderGraph) synthesises barriers in these terms and
 * RHICommandBuffer::Barrier records them; a backend translates them (Vulkan:
 * VulkanTypes.h, to v2 or v1 sync), so nothing above Platform/ writes
 * VkPipelineStageFlags2.
 */
#include "UHE/Core/Core.h"

namespace UHE::RHI
{

/// Pipeline stage where an operation happens (subset of VkPipelineStageFlagBits2).
enum class Stage : u64
{
    None = 0,
    TopOfPipe,
    Transfer,
    Compute,
    Vertex,
    Fragment,
    ColorOutput,
    DepthEarly,
    DepthLate,
    /// Both fragment-test stages: a depth attachment is loaded or cleared at
    /// the early tests and written at the late ones, so a barrier for it must
    /// cover both (sync validation: WRITE_AFTER_WRITE at EARLY_FRAGMENT_TESTS).
    DepthTests,
    BottomOfPipe,
    /// Every stage of everything submitted before: the source of a resource's
    /// first barrier in a frame, which may follow the previous frame's writes
    /// to the same pooled image, or the swapchain acquire.
    AllCommands,
    /// Reading indirect draw/dispatch arguments.
    DrawIndirect,
    /// The CPU: the destination of a barrier before ReadBuffer, the source after a mapped write.
    Host,
};

/// Memory access kind guarded by a barrier (subset of VkAccessFlagBits2).
enum class Access : u64
{
    None = 0,
    TransferRead,
    TransferWrite,
    ShaderRead,
    ShaderWrite,
    ColorWrite,
    DepthWrite,
    MemoryRead,
    MemoryWrite,
    /// Indirect draw/dispatch arguments.
    IndirectRead,
    HostRead,
    HostWrite,
};

/// Logical image layout a resource is in, in the engine's terms.
enum class ImageState : u8
{
    Undefined = 0,
    ColorAttachment,
    DepthAttachment,
    ShaderRead,
    TransferSrc,
    TransferDst,
    Present,
    Storage,
};

} // namespace UHE::RHI
