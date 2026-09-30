// GPU buffer creation and teardown for model geometry.
//
// The only place in the loader that touches the RHI. Geometry is uploaded once
// per glTF mesh after the scene walk, rather than once per node, so a mesh
// referenced by twenty nodes costs one vertex buffer instead of twenty.

#include "uhepch.h"
#include "LoadModel.h"
#include "UHE/RHI/RHICommadBuffer.h"
#include "UHE/Renderer/Renderer.h"

namespace UHE::RD3d
{

void Model::UploadGeometry(Geometry& geometry)
{
    if (geometry.primitive.empty())
        return;

    auto& device = Renderer::GetDevice();
    auto& cmd = device.GetCurrentCommandBuffer();

    for (auto& prim : geometry.primitive)
    {
        if (prim.vertices.empty())
            continue;

        RHI::BufferDesc vbDesc{};
        vbDesc.size = prim.vertices.size() * sizeof(Vertex);
        vbDesc.usage = RHI::BufferUsage::Vertex;
        // hostVisible keeps the data CPU-writable. Correct for the GFX9 floor;
        // a staging -> device-local path for large meshes is future work.
        vbDesc.hostVisible = true;
        prim.VertexBuffer = device.CreateBuffer(vbDesc);
        cmd.UpdateBuffer(prim.VertexBuffer, prim.vertices.data(), vbDesc.size);

        if (!prim.indices.empty())
        {
            RHI::BufferDesc ibDesc{};
            ibDesc.size = prim.indices.size() * sizeof(u32);
            ibDesc.usage = RHI::BufferUsage::Index;
            ibDesc.hostVisible = true;
            prim.IndexBuffer = device.CreateBuffer(ibDesc);
            cmd.UpdateBuffer(prim.IndexBuffer, prim.indices.data(), ibDesc.size);
        }
    }
}

void Model::ReleaseGeometryBuffers(std::vector<Geometry>& geometry)
{
    auto& device = Renderer::GetDevice();

    for (auto& geom : geometry)
    {
        for (auto& prim : geom.primitive)
        {
            if (prim.VertexBuffer)
            {
                device.DestroyBuffer(prim.VertexBuffer);
                prim.VertexBuffer = nullptr;
            }
            if (prim.IndexBuffer)
            {
                device.DestroyBuffer(prim.IndexBuffer);
                prim.IndexBuffer = nullptr;
            }
        }
    }
}

} // namespace UHE::RD3d
