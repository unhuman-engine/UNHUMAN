#pragma once

#include <string>
#include <vector>
#include "UHE/RHI/RenderGraph/RenderGraphBuilder.h"
#include "UHE/RHI/RenderGraph/RenderGraphCompiler.h"

namespace UHE::RHI
{

/**
 * @brief Reuses the last compiled frame while the declared frame is structurally the same.
 *
 * A frame is redeclared every frame, and for a stable scene it compiles to the
 * same passes, edges and barriers each time. The cache compares an exact key,
 * not a hash: every pass's type, queue and accesses (slot, version, role as an
 * attachment), every resource's description, import identity and states, and
 * every name. The topology hash is too coarse for this (it counts a pass's
 * reads instead of naming them), and a collision would replay the wrong
 * barriers.
 *
 * Slot indices are stable for a structurally equal frame, but generations
 * are not: each Reset() hands out new ones. A hit therefore returns the cached
 * frame with every handle rebound to the current generation of its slot.
 * Failed compiles are never cached.
 */
class RenderGraphCompileCache
{
public:
    /// @brief The compiled frame for \p builder's declarations, from the cache when possible.
    [[nodiscard]] RGCompileResult Compile(const RenderGraphBuilder& builder, const RGCompileOptions& options = {});

    [[nodiscard]] u64 Hits() const { return m_Hits; }
    [[nodiscard]] u64 Misses() const { return m_Misses; }
    /// @brief Forgets the cached frame; the next Compile() compiles.
    void Invalidate() { m_Valid = false; }

private:
    struct Key
    {
        std::vector<u64> Words;
        std::vector<std::string> Names;
        [[nodiscard]] bool operator==(const Key& other) const { return Words == other.Words && Names == other.Names; }
    };

    [[nodiscard]] static Key BuildKey(const RenderGraphBuilder& builder, const RGCompileOptions& options);

    Key m_Key;
    RGCompiledFrame m_Frame;
    bool m_Valid = false;
    u64 m_Hits = 0;
    u64 m_Misses = 0;
};

} // namespace UHE::RHI
