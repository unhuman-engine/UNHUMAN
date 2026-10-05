// CPU-side packing of a loaded material into the GPU buffer layout.
//
// In its own translation unit - no RHI, no Vulkan - so the headless test
// harness can verify that what the loader parsed actually lands in the struct
// the shader reads. The failure this guards against is the same silent class
// as the rest of the issue: a field parsed, packed into the wrong slot, and
// rendered wrong with no error anywhere.

#include "MaterialGPU.h"

#include "UHE/Renderer3D/LoadModel.h"

#include <algorithm>

namespace UHE::RD3d
{

MaterialGPU FillMaterialGPU(const Material& material)
{
    MaterialGPU gpu{};
    const auto& ext = material.Extensions;

    gpu.baseColorFactor = material.BaseColorFactor;
    gpu.emissiveFactorStrength = glm::vec4(material.EmissiveFactor, ext.EmissiveStrength);
    gpu.specularColorFactor = glm::vec4(ext.SpecularColorFactor, 1.0f);
    gpu.sheenColorFactor = glm::vec4(ext.SheenColorFactor, 1.0f);
    gpu.attenuationColor = glm::vec4(ext.AttenuationColor, 1.0f);
    gpu.diffuseTransmissionColor = glm::vec4(ext.DiffuseTransmissionColor, 1.0f);

    gpu.metallicFactor = material.MetallicFactor;
    gpu.roughnessFactor = material.RoughnessFactor;
    gpu.normalScale = material.NormalScale;
    gpu.occlusionStrength = material.OcclusionStrength;
    gpu.alphaCutoff = material.AlphaCutoff;
    gpu.ior = ext.IOR;
    gpu.specularFactor = ext.SpecularFactor;
    gpu.clearcoatFactor = ext.ClearcoatFactor;
    gpu.clearcoatRoughnessFactor = ext.ClearcoatRoughnessFactor;
    gpu.clearcoatNormalScale = ext.ClearcoatNormalScale;
    gpu.sheenRoughnessFactor = ext.SheenRoughnessFactor;
    gpu.transmissionFactor = ext.TransmissionFactor;
    gpu.thicknessFactor = ext.ThicknessFactor;
    gpu.attenuationDistance = ext.AttenuationDistance;
    gpu.diffuseTransmissionFactor = ext.DiffuseTransmissionFactor;
    gpu.iridescenceFactor = ext.IridescenceFactor;
    gpu.iridescenceIor = ext.IridescenceIOR;
    gpu.iridescenceThicknessMinimum = ext.IridescenceThicknessMinimum;
    gpu.iridescenceThicknessMaximum = ext.IridescenceThicknessMaximum;
    gpu.anisotropyStrength = ext.AnisotropyStrength;
    gpu.anisotropyRotation = ext.AnisotropyRotation;

    uint32_t features = 0;
    if (ext.HasClearcoat) features |= kFeatureClearcoat;
    if (ext.HasSpecular) features |= kFeatureSpecular;
    if (ext.HasSheen) features |= kFeatureSheen;
    if (ext.HasTransmission) features |= kFeatureTransmission;
    // Volume rides on KHR_materials_volume data; a thickness of 0 attenuates
    // nothing, so the shader only needs the bit when there is something to
    // attenuate with.
    if (ext.HasTransmission && ext.ThicknessFactor > 0.0f) features |= kFeatureVolume;
    if (ext.HasIridescence) features |= kFeatureIridescence;
    if (ext.HasAnisotropy) features |= kFeatureAnisotropy;
    if (ext.HasDiffuseTransmission) features |= kFeatureDiffuseTransmission;

    gpu.flags = glm::ivec4(static_cast<int>(material.Alpha), ext.Unlit ? 1 : 0,
                           static_cast<int>(features), 0);

    // Texture slots. Each carries its own (already engine-space) UV transform,
    // so KHR_texture_transform applies per map, not per material.
    const Ref<Texture2D> slotTextures[] = {
        material.AlbedoTexture,             material.MetallicRoughnessTexture,
        material.NormalTexture,             material.OcclusionTexture,
        material.EmissiveTexture,           ext.ClearcoatTexture,
        ext.ClearcoatRoughnessTexture,      ext.ClearcoatNormalTexture,
        ext.SpecularTexture,                ext.SpecularColorTexture,
        ext.SheenColorTexture,              ext.SheenRoughnessTexture,
        ext.TransmissionTexture,            ext.ThicknessTexture,
        ext.IridescenceTexture,             ext.IridescenceThicknessTexture,
        ext.AnisotropyTexture,              ext.DiffuseTransmissionTexture,
        ext.DiffuseTransmissionColorTexture,
    };
    static_assert(std::size(slotTextures) == kSlotCount, "slot table must cover every GPU slot");

    for (int slot = 0; slot < kSlotCount; ++slot)
    {
        auto& out = gpu.slots[slot];
        const auto& transform = material.UVTransforms[slot];
        if (slotTextures[slot])
            out.index = glm::ivec4(static_cast<int>(slotTextures[slot]->GetTextureIndex()), 0, 0, 0);
        else
            out.index = glm::ivec4(-1, 0, 0, 0);
        out.rotationScale =
            glm::vec4(transform.cosRotation, transform.sinRotation, transform.scale.x, transform.scale.y);
        out.offset = glm::vec4(transform.offset, 0.0f, 0.0f);
    }

    return gpu;
}

} // namespace UHE::RD3d
