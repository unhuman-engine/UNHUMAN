// The GPU entry points the model loader links against. uhe_export cooks with
// ModelLoadOptions::upload off, so none of these is ever reached; each fails
// loudly if a change makes cooking touch the GPU.
#include <cstdio>
#include <cstdlib>
#include "UHE/Renderer/Texture.h"
#include "UHE/Renderer3D/LoadModel.h"

namespace
{
[[noreturn]] void Unreachable(const char* what)
{
    std::fprintf(stderr, "uhe_export: %s called while cooking, which must not touch the GPU\n", what);
    std::abort();
}
} // namespace

namespace UHE
{
Ref<Texture2D> Texture2D::Create(const std::string&, const RHI::SamplerDesc&)
{
    Unreachable("Texture2D::Create");
}
Ref<Texture2D> Texture2D::Create(u32, u32, const RHI::SamplerDesc&)
{
    Unreachable("Texture2D::Create");
}
Ref<Texture2D> Texture2D::CreateFromMemory(const void*, size_t, const RHI::SamplerDesc&)
{
    Unreachable("Texture2D::CreateFromMemory");
}
} // namespace UHE

namespace UHE::RD3d
{
void Model::UploadGeometry(Geometry&, size_t)
{
    Unreachable("Model::UploadGeometry");
}
void Model::ReleaseGeometryBuffers(std::vector<Geometry>&) {}
} // namespace UHE::RD3d
