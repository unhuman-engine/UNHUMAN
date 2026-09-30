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

// The real implementations live in LoadModelUpload.cpp and create Vulkan
// buffers. The harness never uploads, so these are inert.
void Model::UploadGeometry(Geometry& geometry)
{
    std::printf("STUB: Model::UploadGeometry() called - harness expects no GPU upload\n");
}

void Model::ReleaseGeometryBuffers(std::vector<Geometry>& geometry)
{
    // Silent on purpose: Destroy() calls this on every unload, including the
    // normal end-of-scope path after a successful load.
}

} // namespace UHE::RD3d
