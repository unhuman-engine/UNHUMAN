// Stubs for the GPU-facing entry points the loader calls, so the glTF harness can
// link and run without a Vulkan device.
//
// The test assets declare no materials and no textures, and the harness only
// exercises the CPU-side scene walk, so none of these are ever reached in a
// passing run. They exist to satisfy the linker, and each one loudly reports if
// it is ever actually called - a silent no-op here would let a future test
// "pass" while skipping the code it claims to cover.
//
// Test-only: never compile into the engine.

#include <cstdio>

#include "UHE/RHI/RHITypes.h"
#include "UHE/Renderer/Texture.h"
#include "UHE/Renderer3D/LoadModel.h"

namespace UHE
{

Ref<Texture2D> Texture2D::Create(const std::string& path)
{
    std::printf("STUB: Texture2D::Create(\"%s\") called - the glTF harness must not load textures\n", path.c_str());
    return nullptr;
}

Ref<Texture2D> Texture2D::Create(u32 width, u32 height)
{
    std::printf("STUB: Texture2D::Create(%u, %u) called\n", width, height);
    return nullptr;
}

Ref<Texture2D> Texture2D::CreateFromMemory(const void* data, size_t size)
{
    std::printf("STUB: Texture2D::CreateFromMemory(%zu bytes) called\n", size);
    return nullptr;
}

} // namespace UHE

namespace UHE::RD3d
{

// The real implementation creates Vulkan buffers; the harness has no device, so
// this emulates upload by marking the owning Geometry's primitives as uploaded.
//
// Emulating rather than stubbing out is the point: the loader's scene walk copies
// GPU handles into the per-node Mesh entries BEFORE any upload runs, so those
// copies are all null. The real code closes that gap after upload; a stub that
// did nothing here would let that ordering bug pass 60 checks while the editor
// silently draws nothing.
void Model::UploadGeometry(Geometry& geometry, size_t geometryIndex)
{
    for (auto& prim : geometry.primitive)
    {
        if (prim.vertices.empty())
            continue;
        // Non-null handles stand in for Vulkan BufferHandle values.
        prim.VertexBuffer = reinterpret_cast<RHI::BufferHandle>(0x1);
        prim.IndexCount = static_cast<u32>(prim.indices.size());
        if (!prim.indices.empty())
            prim.IndexBuffer = reinterpret_cast<RHI::BufferHandle>(0x2);
    }

    // Same propagation the real upload performs.
    for (auto& mesh : m_LoadedMeshes)
    {
        if (mesh.geometryIndex != geometryIndex)
            continue;
        for (size_t i = 0; i < mesh.primitive.size() && i < geometry.primitive.size(); ++i)
        {
            mesh.primitive[i].VertexBuffer = geometry.primitive[i].VertexBuffer;
            mesh.primitive[i].IndexBuffer = geometry.primitive[i].IndexBuffer;
            mesh.primitive[i].IndexCount = geometry.primitive[i].IndexCount;
        }
    }
}

void Model::ReleaseGeometryBuffers(std::vector<Geometry>& geometry)
{
    // Silent on purpose: Destroy() calls this on every unload, including the
    // normal end-of-scope path after a successful load.
}

} // namespace UHE::RD3d
