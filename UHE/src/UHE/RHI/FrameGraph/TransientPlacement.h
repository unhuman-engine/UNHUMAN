#pragma once
/**
 * @file TransientPlacement.h
 * @brief Packs a frame's transient textures into shared memory blocks by lifetime (issue #32).
 *
 * Textures whose pass intervals do not overlap never hold live data at the
 * same time, so they can occupy the same memory whatever their shapes: the
 * shadow atlas, dead after the shadow passes, and the bloom chain, born after
 * the scene, take one block. Each block is as large as its largest member, and
 * every member starts at offset 0 in it. That is safe because a resource's
 * first barrier in a frame discards its contents and waits on all earlier work.
 *
 * Backend-neutral: sizes, alignments and memory types come from the backend
 * (Vulkan: vkGetDeviceImageMemoryRequirements), and tests/framegraph drives
 * the packing with made-up ones.
 */
#include <span>
#include <vector>
#include "UHE/Core/Core.h"

namespace UHE::RHI
{

struct PlacementRequest
{
    u64 Size = 0;
    u64 Alignment = 1;
    u32 MemoryTypeBits = ~0u; ///< the memory types it may live in
    u32 First = 0;            ///< first pass that uses it
    u32 Last = 0;             ///< last pass that uses it
};

struct PlacementBlock
{
    u64 Size = 0;
    u64 Alignment = 1;
    u32 MemoryTypeBits = ~0u; ///< types every member accepts
    std::vector<u32> Members; ///< request indices
};

struct Placement
{
    std::vector<PlacementBlock> Blocks;
    std::vector<u32> BlockOf; ///< per request, its block
    u64 RequestedBytes = 0;   ///< what one allocation per texture would take
    u64 AllocatedBytes = 0;   ///< what the blocks take
};

/**
 * @brief Largest first, each texture into the first block whose members are all
 *        dead while it lives and whose memory types it accepts; else a new block.
 */
UHE_API Placement PlanTransientPlacement(std::span<const PlacementRequest> requests);

} // namespace UHE::RHI
