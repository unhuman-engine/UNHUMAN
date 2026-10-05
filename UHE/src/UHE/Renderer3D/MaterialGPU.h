// GPU-side material layout, shared by Renderer3D (C++) and Basic3D.slang.
//
// Issue #29 Tier 2: the PBR extension parameters are parsed into
// RD3d::Material but the 256-byte push-constant block that carried material
// fields was already full, so they never reached the renderer. This struct is
// the replacement: one entry per material in a bindless StructuredBuffer, and
// the push constants shrink to carry only WHERE the material lives (buffer
// index + slot).
//
// Layout contract with Basic3D.slang:
//   - every member is a vec4/ivec4 or a plain float/int, so std140 and std430
//     agree and no field needs an offset attribute;
//   - the static_asserts pin the C++ side; the Slang struct must stay
//     field-for-field identical, and the shader test in tests/shader fails the
//     build when the two drift.
//
// One entry is ~1.1KB, dominated by the texture slot table; a scene with a few
// hundred materials costs a few hundred KB of device memory, which is fine at
// this scale and buys per-slot UV transforms for every map.

#pragma once

#include <cstddef>
#include <cstdint>

#include <glm/glm.hpp>

#include "UHE/Core/Core.h"

namespace UHE::RD3d
{

// Order of the per-material texture slot table. Must match the slot indices
// used by the shader's SampleSlot calls and MaterialTextureSlot in LoadModel.h
// (which shares these numeric values by design - one enum is mirrored by the
// other so the shader does not include engine headers).
enum : int
{
    kSlotAlbedo = 0,
    kSlotMetallicRoughness = 1,
    kSlotNormal = 2,
    kSlotOcclusion = 3,
    kSlotEmissive = 4,
    kSlotClearcoat = 5,
    kSlotClearcoatRoughness = 6,
    kSlotClearcoatNormal = 7,
    kSlotSpecular = 8,
    kSlotSpecularColor = 9,
    kSlotSheenColor = 10,
    kSlotSheenRoughness = 11,
    kSlotTransmission = 12,
    kSlotThickness = 13,
    kSlotIridescence = 14,
    kSlotIridescenceThickness = 15,
    kSlotAnisotropy = 16,
    kSlotDiffuseTransmission = 17,
    kSlotDiffuseTransmissionColor = 18,
    kSlotCount = 19,
};

// Bits for MaterialGPU::flags.z. A bit is set only when the file actually
// declared the extension; the shader branches on these instead of on factors
// whose spec defaults (sheen roughness 0, attenuation distance 0) would
// misbehave if treated as "enabled".
enum MaterialFeatureBits : uint32_t
{
    kFeatureClearcoat = 1u << 0,
    kFeatureSpecular = 1u << 1,
    kFeatureSheen = 1u << 2,
    kFeatureTransmission = 1u << 3,
    kFeatureVolume = 1u << 4,
    kFeatureIridescence = 1u << 5,
    kFeatureAnisotropy = 1u << 6,
    kFeatureDiffuseTransmission = 1u << 7,
};

struct GPUSlot
{
    glm::ivec4 index;        // x = bindless texture index, -1 when absent
    glm::vec4 rotationScale; // (cos, sin, scaleX, scaleY) in engine UV space
    glm::vec4 offset;        // (offsetX, offsetY, unused, unused)
};
static_assert(sizeof(GPUSlot) == 48, "GPUSlot layout must match Basic3D.slang");

struct MaterialGPU
{
    glm::vec4 baseColorFactor;          // 0: rgb + alpha
    glm::vec4 emissiveFactorStrength;   // 16: rgb, a = emissiveStrength
    glm::vec4 specularColorFactor;      // 32
    glm::vec4 sheenColorFactor;         // 48
    glm::vec4 attenuationColor;         // 64
    glm::vec4 diffuseTransmissionColor; // 80

    float metallicFactor;              // 96
    float roughnessFactor;             // 100
    float normalScale;                 // 104
    float occlusionStrength;           // 108
    float alphaCutoff;                 // 112
    float ior;                         // 116
    float specularFactor;              // 120
    float clearcoatFactor;             // 124
    float clearcoatRoughnessFactor;    // 128
    float clearcoatNormalScale;        // 132
    float sheenRoughnessFactor;        // 136
    float transmissionFactor;          // 140
    float thicknessFactor;             // 144
    float attenuationDistance;         // 148
    float diffuseTransmissionFactor;   // 152
    float iridescenceFactor;           // 156
    float iridescenceIor;              // 160
    float iridescenceThicknessMinimum; // 164
    float iridescenceThicknessMaximum; // 168
    float anisotropyStrength;          // 172
    float anisotropyRotation;          // 176
    float padding0;                    // 180
    float padding1;                    // 184
    float padding2;                    // 188

    glm::ivec4 flags;                  // 192: x = alphaMode, y = unlit, z = feature bits, w = spare

    GPUSlot slots[kSlotCount];         // 208 .. 1120
};
static_assert(offsetof(MaterialGPU, flags) == 192, "MaterialGPU layout must match Basic3D.slang");
static_assert(offsetof(MaterialGPU, slots) == 208, "MaterialGPU layout must match Basic3D.slang");
static_assert(sizeof(MaterialGPU) == 208 + kSlotCount * 48, "MaterialGPU layout must match Basic3D.slang");

} // namespace UHE::RD3d
