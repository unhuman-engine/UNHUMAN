#include "UHE/RHI/FrameGraph/TransientPlacement.h"
#include <algorithm>
#include <numeric>

namespace UHE::RHI
{

Placement PlanTransientPlacement(std::span<const PlacementRequest> requests)
{
    Placement placement;
    placement.BlockOf.assign(requests.size(), 0);
    std::vector<u32> order(requests.size());
    std::iota(order.begin(), order.end(), 0u);
    // Largest first: a block's size is its first member's, and later members fit inside.
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return requests[a].Size > requests[b].Size; });

    const auto overlaps = [](const PlacementRequest& a, const PlacementRequest& b)
    { return a.First <= b.Last && b.First <= a.Last; };

    for (const u32 index : order)
    {
        const PlacementRequest& request = requests[index];
        placement.RequestedBytes += request.Size;
        u32 chosen = static_cast<u32>(placement.Blocks.size());
        for (u32 b = 0; b < placement.Blocks.size(); ++b)
        {
            const PlacementBlock& block = placement.Blocks[b];
            if ((block.MemoryTypeBits & request.MemoryTypeBits) == 0)
                continue;
            const bool free = std::none_of(block.Members.begin(), block.Members.end(),
                                           [&](u32 member) { return overlaps(requests[member], request); });
            if (free)
            {
                chosen = b;
                break;
            }
        }
        if (chosen == placement.Blocks.size())
            placement.Blocks.emplace_back();
        PlacementBlock& block = placement.Blocks[chosen];
        block.Size = std::max(block.Size, request.Size);
        block.Alignment = std::max(block.Alignment, request.Alignment);
        block.MemoryTypeBits &= request.MemoryTypeBits;
        block.Members.push_back(index);
        placement.BlockOf[index] = chosen;
    }
    for (const PlacementBlock& block : placement.Blocks)
        placement.AllocatedBytes += block.Size;
    return placement;
}

} // namespace UHE::RHI
