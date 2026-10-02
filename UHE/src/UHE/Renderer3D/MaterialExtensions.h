// Material extension data for the PBR materials of issue #29 Tier 2.
//
// Split from the core metallic-roughness fields, which stay on the material
// struct, because these are OPTIONAL extensions: a file that declares none of
// them must render exactly as it did before, and keeping them in one aggregate
// makes "which extensions did this material actually use" a single question
// rather than a dozen.
//
// Defaults here are the spec defaults, not zero. That distinction is the whole
// point of most of these fields: a zero initialiser silently disables the
// feature it names (a clearcoat of 0 means no clearcoat, but a sheen roughness
// of 0 means mirror-sheen, and an attenuation distance of 0 means fully opaque),
// so each is defaulted to what the spec says the value is when the extension is
// absent.
//
// Texture references are indices into Model::GetMaterials()' texture slots, -1
// when the map is not declared.

#pragma once
#include <glm/glm.hpp>
#include <limits>

#include "UHE/Core/Core.h"
#include "UHE/Renderer/Texture.h"

namespace UHE::RD3d
{

// How a KHR_materials_transmission surface is composited. The extension needs a
// sorted pass to look right; until one exists, the renderer draws these in
// submission order like any other BLEND material and says so at load time.
enum class BlendApproach : u8
{
    Opaque = 0,
    // Drawn in the opaque pass, ignoring transmission. Correct for a thin
    // surface with a small factor, wrong but stable.
    OpaqueWithTransmission = 1,
    // Drawn after opaques, blended. Needs depth sorting to avoid artefacts where
    // surfaces overlap.
    Blend = 2,
};

struct MaterialExtensions
{
    // ── KHR_materials_emissive_strength ─────────────────────────────────
    // Multiplies emissiveFactor and emissiveTexture. Spec default 1.0. A zero
    // default here would erase every emissive surface in every asset.
    float EmissiveStrength = 1.0f;

    // ── KHR_materials_ior ────────────────────────────────────────────────
    // Index of refraction. Spec default 1.5, which is the value used for
    // dielectric F0. Also the denominator for transmission refraction.
    float IOR = 1.5f;

    // ── KHR_materials_clearcoat ──────────────────────────────────────────
    // A second, thinner specular lobe over the base. clearcoatFactor 0 means no
    // clearcoat; clearcoatRoughnessFactor defaults to 0, which the spec defines
    // as a mirror finish, so it is 0 rather than 0.5 on purpose.
    float ClearcoatFactor = 0.0f;
    float ClearcoatRoughnessFactor = 0.0f;
    float ClearcoatNormalScale = 1.0f;
    bool HasClearcoat = false;
    // Clearcoat factor and roughness are SCALARS, so the maps carrying them are
    // linear data - the same reason normal maps are.
    Ref<Texture2D> ClearcoatTexture = nullptr;
    Ref<Texture2D> ClearcoatRoughnessTexture = nullptr;
    Ref<Texture2D> ClearcoatNormalTexture = nullptr;

    // ── KHR_materials_specular ───────────────────────────────────────────
    // Replaces the fixed 0.04 dielectric F0. specularFactor 1.0 is the
    // spec default and reproduces that fixed value, so a material without the
    // extension is unchanged.
    float SpecularFactor = 1.0f;
    glm::vec3 SpecularColorFactor = glm::vec3(1.0f);
    bool HasSpecular = false;
    // specularTexture scales a scalar factor - linear. specularColorTexture is
    // a colour - sRGB, which is the lambda's default.
    Ref<Texture2D> SpecularTexture = nullptr;
    Ref<Texture2D> SpecularColorTexture = nullptr;

    // ── KHR_materials_sheen ──────────────────────────────────────────────
    // A retroreflective fabric layer. sheenColorFactor defaults to BLACK per
    // spec, which means no sheen; sheenRoughnessFactor defaults to 0.
    glm::vec3 SheenColorFactor = glm::vec3(0.0f);
    float SheenRoughnessFactor = 0.0f;
    bool HasSheen = false;
    // sheenColorTexture is a colour (sRGB); sheenRoughnessTexture scales a
    // scalar (linear).
    Ref<Texture2D> SheenColorTexture = nullptr;
    Ref<Texture2D> SheenRoughnessTexture = nullptr;

    // ── KHR_materials_transmission + KHR_materials_volume ────────────────
    // transmissionFactor 0 means an opaque surface. attenuationDistance
    // defaults to INFINITY, meaning light travels indefinitely without
    // attenuation - NOT 0, which would make the material fully opaque.
    float TransmissionFactor = 0.0f;
    float ThicknessFactor = 0.0f;
    float AttenuationDistance = std::numeric_limits<float>::infinity();
    glm::vec3 AttenuationColor = glm::vec3(1.0f);
    bool HasTransmission = false;
    // Both scale scalars - linear.
    Ref<Texture2D> TransmissionTexture = nullptr;
    Ref<Texture2D> ThicknessTexture = nullptr;

    // ── KHR_materials_iridescence ────────────────────────────────────────
    // Thin-film interference. factor 0 means none; the thickness range is in
    // nanometres and defaults to 100-400.
    float IridescenceFactor = 0.0f;
    float IridescenceIOR = 1.3f;
    float IridescenceThicknessMinimum = 100.0f;
    float IridescenceThicknessMaximum = 400.0f;
    bool HasIridescence = false;
    // Both scale scalars - linear.
    Ref<Texture2D> IridescenceTexture = nullptr;
    Ref<Texture2D> IridescenceThicknessTexture = nullptr;

    // ── KHR_materials_anisotropy ─────────────────────────────────────────
    // Directional roughness for brushed metal. Strength 0 is anisotropic-free.
    float AnisotropyStrength = 0.0f;
    float AnisotropyRotation = 0.0f;
    bool HasAnisotropy = false;
    // Scales a scalar strength - linear.
    Ref<Texture2D> AnisotropyTexture = nullptr;

    // ── KHR_materials_unlit ──────────────────────────────────────────────
    // Bypass shading entirely and output the base colour. Tier 3 in the issue,
    // but it is a material flag with no data of its own, so it is carried here
    // rather than in a separate pass.
    bool Unlit = false;

    // How a transmission surface is drawn. Chosen by the loader from the
    // material's alpha mode and transmission factor, so the renderer does not
    // have to re-derive it per draw.
    BlendApproach TransmissionBlend = BlendApproach::OpaqueWithTransmission;
};

} // namespace UHE::RD3d