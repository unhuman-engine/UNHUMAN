#pragma warning(disable: 41012)
// 3D model shader (PBR + glTF material extensions, issue #29).
//
// Material parameters live in a per-material StructuredBuffer (MaterialGPU.h
// owns the layout contract), so every Tier 2 extension can shade without
// growing push constants - the old block sat exactly at the 256-byte device
// limit and physically could not carry one more field. Push constants now hold
// only where the material lives and where the lights/bones live.
//
// What shades here, beyond base metallic-roughness:
//   KHR_materials_ior / _specular    - dielectric F0 (spec default = 0.04 white)
//   KHR_materials_clearcoat          - second GGX lobe with its own normal map
//   KHR_materials_sheen              - Charlie-distribution fabric lobe
//   KHR_materials_anisotropy         - anisotropic GGX (Burley) NDF
//   KHR_materials_iridescence        - thin-film interference on the specular lobe
//   KHR_materials_transmission(+volume) - energy split + blend alpha, volume attenuation
//   KHR_materials_diffuse_transmission  - light diffused through thin surfaces (leaves)
//   KHR_materials_emissive_strength  - emissive multiplier
//   KHR_materials_unlit              - shading bypass
//   KHR_texture_transform            - per-slot UV transform, engine-space
//   spot lights (KHR_lights_punctual and scene spot components) - cone falloff
//
// Known approximations, deliberate until a proper transmission pass exists:
// transmission alpha-blends against the backbuffer in submission order, and
// volume attenuation only tints the transmitted diffuse term, not the sampled
// background.

struct VertexInput {
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(1)]] float3 a_Normal : NORMAL;
    [[vk::location(2)]] float2 a_TexCoord : TEXCOORD;
    // xyz = tangent, w = handedness sign. Added for normal mapping; the loader
    // generates this per-corner and splits seam vertices so w is meaningful.
    [[vk::location(3)]] float4 a_Tangent : TANGENT;
    [[vk::location(4)]] int4 a_Joints : JOINTS;
    [[vk::location(5)]] float4 a_Weights : WEIGHTS;
    // COLOR_0. Location 6. glTF allows vec3 or vec4; the loader normalises both
    // to vec4 on the CPU so the vertex layout needs only one format.
    [[vk::location(6)]] float4 a_Color : COLOR;
};

struct VertexOutput {
    float4 location : SV_Position;
    [[vk::location(0)]] float3 v_Normal;
    [[vk::location(1)]] float2 v_TexCoord;
    [[vk::location(2)]] float3 v_WorldPos;
    [[vk::location(3)]] float3 v_Tangent;
    [[vk::location(4)]] float3 v_Bitangent;
    [[vk::location(5)]] float4 v_Color;
};

struct PushConstants {
    float4x4 viewProjection;
    float4x4 model;
    float4 cameraPos;
    int entityID;               // 144
    int enableLighting;         // 148
    int lightBufferIndex;       // 152
    int numLights;              // 156
    int boneBufferIndex;        // 160
    int boneOffset;             // 164
    int materialBufferIndex;    // 168
    int materialIndex;          // 172
    int useVertexColor;         // 176 - per-PRIMITIVE flag, not per-material
    // An int[3] ARRAY, not int3: std430 gives a vec3 a 16-byte base alignment,
    // which would push this member to offset 192 and grow the block to 208.
    // The array keeps the C++ struct (sizeof 192) and the SPIR-V layout equal.
    int padding[3];             // 180
};                              // 192 bytes

[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

struct LightData {
    float4 type_radius_cones; // x = type (0 dir, 1 point, 2 spot), y = radius, z = cos(inner), w = cos(outer)
    float4 position_or_direction;
    float4 direction_pad;     // spot forward axis
    float4 color_intensity;
};

// ── MaterialGPU (layout contract: UHE/src/UHE/Renderer3D/MaterialGPU.h) ─────

struct GPUSlot {
    int4 index;             // x = bindless texture index, -1 when absent
    float4 rotationScale;   // (cos, sin, scaleX, scaleY) in engine UV space
    float4 offset;          // (offsetX, offsetY, -)
};

struct MaterialGPU {
    float4 baseColorFactor;
    float4 emissiveFactorStrength;
    float4 specularColorFactor;
    float4 sheenColorFactor;
    float4 attenuationColor;
    float4 diffuseTransmissionColor;

    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
    float alphaCutoff;
    float ior;
    float specularFactor;
    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    float clearcoatNormalScale;
    float sheenRoughnessFactor;
    float transmissionFactor;
    float thicknessFactor;
    float attenuationDistance;
    float diffuseTransmissionFactor;
    float iridescenceFactor;
    float iridescenceIor;
    float iridescenceThicknessMinimum;
    float iridescenceThicknessMaximum;
    float anisotropyStrength;
    float anisotropyRotation;
    float padding0;
    float padding1;
    float padding2;

    int4 flags;             // x = alphaMode, y = unlit, z = feature bits, w = spare

    GPUSlot slots[19];
};

// Feature bits, mirroring MaterialFeatureBits in MaterialGPU.h.
static const int kFeatureClearcoat = 1 << 0;
static const int kFeatureSpecular = 1 << 1;
static const int kFeatureSheen = 1 << 2;
static const int kFeatureTransmission = 1 << 3;
static const int kFeatureVolume = 1 << 4;
static const int kFeatureIridescence = 1 << 5;
static const int kFeatureAnisotropy = 1 << 6;
static const int kFeatureDiffuseTransmission = 1 << 7;

[[vk::binding(0, 0)]] StructuredBuffer<LightData> u_LightBuffers[10000];
[[vk::binding(1, 0)]] Sampler2D u_Textures[10000];
[[vk::binding(0, 0)]] StructuredBuffer<float4x4> u_BoneBuffers[10000];
[[vk::binding(2, 0)]] StructuredBuffer<MaterialGPU> u_MaterialBuffers[10000];

struct FragmentOutput {
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};

// ── helpers ──────────────────────────────────────────────────────────────────

// Samples texture slot `slot` at `uv`, applying that slot's KHR_texture_transform
// (engine-space: uv' = R(-theta) * (uv * scale) + offset, see LoadModel.cpp).
// Returns float4(0) when the slot has no texture; callers substitute the
// glTF default for their channel.
float4 SampleSlot(MaterialGPU m, int slot, float2 uv)
{
    GPUSlot s = m.slots[slot];
    if (s.index.x < 0)
        return float4(0.0, 0.0, 0.0, 0.0);
    float2 t = float2(
        s.rotationScale.x * uv.x * s.rotationScale.z + s.rotationScale.y * uv.y * s.rotationScale.w,
        -s.rotationScale.y * uv.x * s.rotationScale.z + s.rotationScale.x * uv.y * s.rotationScale.w);
    t += s.offset.xy;
    return u_Textures[s.index.x].Sample(t);
}

// Schlick fresnel with the glTF F0/F90 split.
float3 FresnelSchlick(float cosTheta, float3 F0, float3 F90)
{
    return F0 + (F90 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// GGX / Trowbridge-Reitz with anisotropic alpha (Burley's form). ab = alpha
// along the bitangent, at = alpha along the (rotated) tangent.
float D_GGXAniso(float at, float ab, float NoH, float ToH, float BoH)
{
    float a2 = at * ab;
    float3 d = float3(ab * ToH, at * BoH, a2 * NoH);
    float d2 = dot(d, d);
    float vis = a2 / (3.14159265359 * d2 * d2);
    return max(vis, 0.0);
}

// Sheen: Charlie distribution + Ashikhmin-style visibility (Khronos approx).
float D_Charlie(float sheenRoughness, float NoH)
{
    float invR = 1.0 / max(sheenRoughness, 1e-4);
    float cos2h = NoH * NoH;
    float sin2h = max(1.0 - cos2h, 1e-4);
    return (2.0 + invR) * pow(sin2h, invR * 0.5) / (2.0 * 3.14159265359);
}

float V_Charlie(float NoV, float NoL, float sheenRoughness)
{
    float lambdaV = NoV < 0.5 ? exp2((log2(1.0 - NoV) - 1.0) * -sheenRoughness) : 0.0;
    float lambdaL = NoL < 0.5 ? exp2((log2(1.0 - NoL) - 1.0) * -sheenRoughness) : 0.0;
    return 1.0 / (1.0 + lambdaV + lambdaL);
}

// KHR_materials_iridescence: two-beam thin-film interference evaluated at the
// RGB primaries. The extension's reference implementation integrates a
// three-layer model over the visible spectrum; this compact version has the
// same hue-vs-thickness and hue-vs-angle behaviour and is only used to tint
// the specular lobe. Thickness is in nanometres.
float3 IridescenceReflectance(float ior, float thicknessNm, float NdotV)
{
    float PI = 3.14159265359;
    float sin2 = clamp(1.0 - NdotV * NdotV, 0.0, 1.0);
    float cosThetaT = sqrt(max(1.0 - sin2 / (ior * ior), 0.0)); // Snell into the film

    float wavelengths[3];
    wavelengths[0] = 650.0;
    wavelengths[1] = 550.0;
    wavelengths[2] = 450.0;

    float3 reflectance = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int k = 0; k < 3; ++k)
    {
        // Optical path difference between the top and bottom reflections, plus
        // the PI flip at the external interface.
        float delta = 4.0 * PI * ior * thicknessNm * cosThetaT / wavelengths[k] + PI;
        reflectance[k] = 0.5 - 0.5 * cos(delta);
    }
    return reflectance;
}

// ── stages ───────────────────────────────────────────────────────────────────

[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;

    float4 localPos = float4(input.a_Position, 1.0);
    float3 localNormal = input.a_Normal;

    if (pc.boneBufferIndex >= 0 && pc.boneOffset >= 0)
    {
        StructuredBuffer<float4x4> boneBuffer = u_BoneBuffers[pc.boneBufferIndex];

        float4x4 skinMat =
            boneBuffer[pc.boneOffset + input.a_Joints.x] * input.a_Weights.x +
            boneBuffer[pc.boneOffset + input.a_Joints.y] * input.a_Weights.y +
            boneBuffer[pc.boneOffset + input.a_Joints.z] * input.a_Weights.z +
            boneBuffer[pc.boneOffset + input.a_Joints.w] * input.a_Weights.w;

        localPos = mul(skinMat, float4(input.a_Position, 1.0));
        localNormal = mul((float3x3)skinMat, input.a_Normal);
    }

    float4 worldPos = mul(pc.model, localPos);
    output.v_WorldPos = worldPos.xyz;
    output.v_Normal = normalize(mul((float3x3)pc.model, localNormal));
    output.v_TexCoord = input.a_TexCoord;

    // Tangent frame in world space. The bitangent's handedness comes from the
    // loader-generated w component, which flips on UV-mirrored seams; without it
    // normal maps light the wrong way on half the surface.
    float3 worldTangent = mul((float3x3)pc.model, input.a_Tangent.xyz);
    output.v_Tangent = normalize(worldTangent);
    output.v_Bitangent = normalize(cross(output.v_Normal, output.v_Tangent) * input.a_Tangent.w);

    // COLOR_0 passes through unchanged. It is linear data in glTF and is NOT
    // gamma-decoded - decoding it would darken every vertex-painted asset.
    output.v_Color = input.a_Color;

    output.location = mul(pc.viewProjection, worldPos);
    return output;
}

[shader("fragment")]
FragmentOutput fragmentMain(VertexOutput input, bool isFrontFace : SV_IsFrontFace) {
    FragmentOutput output;

    MaterialGPU m = u_MaterialBuffers[pc.materialBufferIndex][pc.materialIndex];
    const int features = m.flags.z;

    // baseColor = texture (through its own UV transform) x factor x COLOR_0.
    // A slot with no texture reads white, which keeps a factor-only material
    // working: glTF's default base texture is a 1x1 opaque white image.
    float4 albedoTex = (m.slots[0].index.x >= 0) ? SampleSlot(m, 0, input.v_TexCoord)
                                                 : float4(1.0, 1.0, 1.0, 1.0);
    float4 baseColor = albedoTex * m.baseColorFactor;
    if (pc.useVertexColor == 1)
        baseColor *= input.v_Color;

    // Transmission takes part of the outgoing light "through" the surface, so
    // it scales the blend alpha (the Khronos reference approximates glass as
    // alpha = 1 - transmission). The factor map rides in the R channel.
    float transmission = 0.0;
    if ((features & kFeatureTransmission) != 0)
    {
        transmission = m.transmissionFactor;
        if (m.slots[12].index.x >= 0)
            transmission *= SampleSlot(m, 12, input.v_TexCoord).r;
    }

    // Alpha MASK: discard below the cutoff. BLEND blends in submission order
    // until a sorted transparent pass exists; the loader already warned.
    if (m.flags.x == 1)
    {
        if (baseColor.a < m.alphaCutoff)
            discard;
    }

    // glTF unlit: output the base colour untouched (BLEND keeps its alpha).
    if (m.flags.y == 1)
    {
        float unlitAlpha = (m.flags.x == 2) ? baseColor.a : 1.0;
        output.Color = float4(baseColor.rgb, unlitAlpha);
        output.EntityID = pc.entityID;
        return output;
    }

    float3 finalColor = baseColor.rgb;
    if (pc.enableLighting == 1)
    {
        float3 normal = normalize(input.v_Normal);
        if (!isFrontFace)
        {
            normal = -normal;
            input.v_Tangent = -input.v_Tangent;
            input.v_Bitangent = -input.v_Bitangent;
        }


        // Normal mapping in tangent space. glTF stores normals in [-1,1] and
        // samples them from a texture, hence the *2-1 on read.
        if (m.slots[2].index.x >= 0)
        {
            float3 tangentNormal = SampleSlot(m, 2, input.v_TexCoord).rgb * 2.0 - 1.0;
            tangentNormal.xy *= m.normalScale;
            normal = normalize(
                tangentNormal.x * input.v_Tangent +
                tangentNormal.y * input.v_Bitangent +
                tangentNormal.z * normal);
        }

        // Clearcoat has its own normal map per the extension.
        float3 clearcoatNormal = normal;
        if ((features & kFeatureClearcoat) != 0 && m.slots[7].index.x >= 0)
        {
            float3 ccNormal = SampleSlot(m, 7, input.v_TexCoord).rgb * 2.0 - 1.0;
            ccNormal.xy *= m.clearcoatNormalScale;
            clearcoatNormal = normalize(
                ccNormal.x * input.v_Tangent +
                ccNormal.y * input.v_Bitangent +
                ccNormal.z * clearcoatNormal);
        }

        float3 V = normalize(pc.cameraPos.xyz - input.v_WorldPos);
        float NdotV = clamp(dot(normal, V), 1e-4, 1.0);

        float metallic = clamp(m.metallicFactor, 0.0, 0.99);
        float roughness = clamp(m.roughnessFactor, 0.04, 0.99);
        if (m.slots[1].index.x >= 0)
        {
            float4 mrTex = SampleSlot(m, 1, input.v_TexCoord);
            metallic = clamp(m.metallicFactor * mrTex.b, 0.0, 0.99);
            roughness = clamp(m.roughnessFactor * mrTex.g, 0.04, 0.99);
        }

        // Dielectric F0: IOR-derived base, scaled by KHR_materials_specular's
        // colour/factor/map. With everything at spec default this is 0.04 white,
        // identical to the fixed value the old shader used.
        float iorF0 = (m.ior - 1.0) / (m.ior + 1.0);
        iorF0 *= iorF0;
        float3 F0_dielectric = iorF0 * m.specularColorFactor.rgb * m.specularFactor;
        if ((features & kFeatureSpecular) != 0 && m.slots[8].index.x >= 0)
            F0_dielectric *= SampleSlot(m, 8, input.v_TexCoord).r;

        float3 F0 = lerp(F0_dielectric, baseColor.rgb, metallic);

        // Per-pixel parameters of the added lobes, sampled once (not per light).
        float3 sheenColor = m.sheenColorFactor.rgb;
        float sheenRoughness = clamp(m.sheenRoughnessFactor, 0.03, 1.0);
        float sheenLuminance = 0.0;
        if ((features & kFeatureSheen) != 0)
        {
            if (m.slots[10].index.x >= 0)
                sheenColor *= SampleSlot(m, 10, input.v_TexCoord).rgb;
            if (m.slots[11].index.x >= 0)
                sheenRoughness = clamp(sheenRoughness * SampleSlot(m, 11, input.v_TexCoord).r, 0.03, 1.0);
            // The sheen layer takes its energy out of the diffuse lobe; the
            // spec's reference scales diffuse by the sheen colour's luminance.
            sheenLuminance = dot(sheenColor, float3(0.2126, 0.7152, 0.0722));
        }

        float clearcoat = 0.0;
        float clearcoatRough = clamp(m.clearcoatRoughnessFactor, 0.04, 0.99);
        if ((features & kFeatureClearcoat) != 0)
        {
            clearcoat = m.clearcoatFactor;
            if (m.slots[5].index.x >= 0)
                clearcoat *= SampleSlot(m, 5, input.v_TexCoord).r;
            if (m.slots[6].index.x >= 0)
                clearcoatRough = clamp(clearcoatRough * SampleSlot(m, 6, input.v_TexCoord).r, 0.04, 0.99);
        }

        float diffuseTransmission = 0.0;
        float3 dtColor = m.diffuseTransmissionColor.rgb;
        if ((features & kFeatureDiffuseTransmission) != 0)
        {
            diffuseTransmission = m.diffuseTransmissionFactor;
            if (m.slots[17].index.x >= 0)
                diffuseTransmission *= SampleSlot(m, 17, input.v_TexCoord).r;
            if (m.slots[18].index.x >= 0)
                dtColor *= SampleSlot(m, 18, input.v_TexCoord).rgb;
        }

        // Iridescence: reflectance curve + factor, evaluated once per pixel.
        float iridescenceFactor = 0.0;
        float3 iridescenceReflectance = float3(0.0, 0.0, 0.0);
        if ((features & kFeatureIridescence) != 0 && m.iridescenceFactor > 0.0)
        {
            iridescenceFactor = m.iridescenceFactor;
            if (m.slots[14].index.x >= 0)
                iridescenceFactor *= SampleSlot(m, 14, input.v_TexCoord).r;
            float thickness = lerp(m.iridescenceThicknessMinimum, m.iridescenceThicknessMaximum, 0.5);
            if (m.slots[15].index.x >= 0)
                thickness = lerp(m.iridescenceThicknessMinimum, m.iridescenceThicknessMaximum,
                                 SampleSlot(m, 15, input.v_TexCoord).r);
            iridescenceReflectance = IridescenceReflectance(m.iridescenceIor, thickness, NdotV);
        }

        // Anisotropy rotates the tangent frame; the map scales the strength.
        float anisotropy = 0.0;
        float anisoRotation = m.anisotropyRotation;
        if ((features & kFeatureAnisotropy) != 0)
        {
            anisotropy = m.anisotropyStrength;
            if (m.slots[16].index.x >= 0)
                anisotropy *= SampleSlot(m, 16, input.v_TexCoord).r;
        }
        float anisoT = 1.0;
        float anisoB = 0.0;
        if (anisotropy > 0.0)
        {
            float c = cos(anisoRotation);
            float s = sin(anisoRotation);
            anisoT = c; // tangent = cos * T0 + sin * B0
            anisoB = s;
        }

        // Volume attenuation for light travelling thickness/attenuationDistance
        // through the medium (KHR_materials_volume). At the spec defaults
        // (attenuationDistance INFINITY) this is exactly 1.
        float3 volumeAttenuation = float3(1.0, 1.0, 1.0);
        if ((features & kFeatureVolume) != 0 && m.attenuationDistance > 0.0)
        {
            float distanceRatio = m.thicknessFactor / m.attenuationDistance;
            volumeAttenuation = pow(m.attenuationColor.rgb,
                                    float3(distanceRatio, distanceRatio, distanceRatio));
        }

        float PI = 3.14159265359;
        float3 Lo = float3(0.0, 0.0, 0.0);

        for (int i = 0; i < pc.numLights; ++i)
        {
            LightData light = u_LightBuffers[pc.lightBufferIndex][i];

            int type = int(light.type_radius_cones.x);
            float radius = max(light.type_radius_cones.y, 0.1); // prevent div by zero

            float3 L;
            float attenuation = 1.0;
            float spotFactor = 1.0;

            if (type == 0)
            { // Directional
                L = normalize(-light.position_or_direction.xyz);
            }
            else if (type == 1)
            { // Point
                L = light.position_or_direction.xyz - input.v_WorldPos;
                float distance = length(L);
                L = normalize(L);
                // Linear/quadratic window falloff (engine convention).
                attenuation = clamp(1.0 - (distance * distance) / (radius * radius), 0.0, 1.0);
                attenuation *= attenuation;
            }
            else
            { // Spot (KHR_lights_punctual + scene spot light components)
                L = light.position_or_direction.xyz - input.v_WorldPos;
                float distance = length(L);
                L = normalize(L);
                attenuation = clamp(1.0 - (distance * distance) / (radius * radius), 0.0, 1.0);
                attenuation *= attenuation;

                // Cone: full inside cos(inner), smooth to zero at cos(outer).
                // Angles arrive as cosines so this is one dot product. The
                // outer cosine is clamped inward so a degenerate file with
                // outer < inner cannot invert the ramp.
                float cosAngle = dot(-L, normalize(light.direction_pad.xyz));
                float cosInner = light.type_radius_cones.z;
                float cosOuter = min(light.type_radius_cones.w, cosInner);
                spotFactor = clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
            }
            if (spotFactor <= 0.0 || attenuation <= 0.0)
                continue;

            float3 radiance = light.color_intensity.xyz * light.color_intensity.w * attenuation * spotFactor * PI;

            float NoL = dot(normal, L);
            float ccNoL = dot(clearcoatNormal, L);
            float NoLdt = dot(-normal, L); // through-surface direction, for diffuse transmission

            if (NoL <= 0.0 && ccNoL <= 0.0 && NoLdt <= 0.0)
                continue;

            float3 H = normalize(V + L);
            float NoH = clamp(dot(normal, H), 0.0, 1.0);
            float NoLc = clamp(NoL, 0.0, 1.0);
            float NoVc = clamp(NdotV, 0.0, 1.0);

            // ── main specular lobe ────────────────────────────────────────
            float alpha = roughness * roughness;
            float NDF;
            if (anisotropy > 0.0)
            {
                float at = max(alpha * (1.0 + anisotropy), 1e-3);
                float3 T = normalize(anisoT * input.v_Tangent + anisoB * input.v_Bitangent);
                float3 B = normalize(anisoB * input.v_Tangent - anisoT * input.v_Bitangent);
                NDF = D_GGXAniso(at, alpha, NoH, dot(T, H), dot(B, H));
            }
            else
            {
                float a2 = alpha * alpha;
                float denom = PI * (NoH * NoH * (a2 - 1.0) + 1.0);
                NDF = a2 / max(denom * denom, 1e-8);
            }

            float r = (roughness + 1.0);
            float k = (r * r) / 8.0;
            float G = (NoVc / (NoVc * (1.0 - k) + k)) * (NoLc / (NoLc * (1.0 - k) + k));
            float3 F = FresnelSchlick(clamp(dot(H, V), 0.0, 1.0), F0, float3(1.0, 1.0, 1.0));
            if (iridescenceFactor > 0.0)
                F = lerp(F, iridescenceReflectance, iridescenceFactor);

            float3 specular = NDF * G * F / max(4.0 * NoVc * NoLc, 1e-4);

            // ── diffuse, with the energy each added layer takes away ──────
            float3 kD = (float3(1.0, 1.0, 1.0) - F) * (1.0 - metallic);
            if (sheenLuminance > 0.0)
                kD *= (1.0 - sheenLuminance);
            if (transmission > 0.0)
                kD *= (1.0 - transmission);
            if (diffuseTransmission > 0.0)
                kD *= (1.0 - diffuseTransmission);

            float3 diffuse = kD * baseColor.rgb / PI;

            Lo += (diffuse + specular) * radiance * NoLc;

            // ── sheen lobe ────────────────────────────────────────────────
            if (sheenLuminance > 0.0 && NoLc > 0.0)
            {
                float Dsheen = D_Charlie(sheenRoughness, NoH);
                float Vsheen = V_Charlie(NoVc, NoLc, sheenRoughness);
                Lo += Dsheen * Vsheen * sheenColor * radiance * NoLc;
            }

            // ── clearcoat lobe (fixed 0.04 F0, own normal + roughness) ────
            if (clearcoat > 0.0 && ccNoL > 0.0)
            {
                float cca = clearcoatRough * clearcoatRough;
                float ccNoH = clamp(dot(clearcoatNormal, H), 0.0, 1.0);
                float ccNoV = clamp(dot(clearcoatNormal, V), 1e-4, 1.0);
                float ccDenom = PI * (ccNoH * ccNoH * (cca * cca - 1.0) + 1.0);
                float ccD = (cca * cca) / max(ccDenom * ccDenom, 1e-8);
                float cck = ((clearcoatRough + 1.0) * (clearcoatRough + 1.0)) / 8.0;
                float ccG = (ccNoV / (ccNoV * (1.0 - cck) + cck)) * (ccNoL / (ccNoL * (1.0 - cck) + cck));
                float ccF = 0.04 + 0.96 * pow(clamp(1.0 - abs(dot(V, H)), 0.0, 1.0), 5.0);
                Lo += clearcoat * ccD * ccG * ccF * radiance * ccNoL / max(4.0 * ccNoV * ccNoL, 1e-4);
            }

            // ── diffuse transmission: light that passes through and re-emerges
            //    diffused from the back face, tinted by the volume medium. ──
            if (diffuseTransmission > 0.0 && NoLdt > 0.0)
            {
                float3 transmitted = diffuseTransmission * dtColor * volumeAttenuation *
                                     (kD * baseColor.rgb / PI);
                Lo += transmitted * radiance * NoLdt;
            }
        }

        // Occlusion is a multiplier on the ambient/diffuse term only; glTF
        // defines it as cavity shading, not as a shadow.
        if (m.slots[3].index.x >= 0)
        {
            float ao = SampleSlot(m, 3, input.v_TexCoord).r;
            ao = lerp(1.0, ao, m.occlusionStrength);
            Lo *= ao;
        }

        // Boost ambient a bit so dark sides are visible
        float3 ambient = float3(0.08, 0.08, 0.08) * baseColor.rgb;
        float3 litColor = ambient + Lo;

        // Emissive is additive and unlit. emissiveStrength multiplies both the
        // factor and the map (KHR_materials_emissive_strength).
        float3 emissive = m.emissiveFactorStrength.rgb * m.emissiveFactorStrength.a;
        if (m.slots[4].index.x >= 0)
            emissive *= SampleSlot(m, 4, input.v_TexCoord).rgb;
        litColor += emissive;

        // Simple HDR tonemapping (Exposure-based instead of Reinhard for better contrast)
        float exposure = 1.0;
        litColor = float3(1.0, 1.0, 1.0) - exp(-litColor * exposure);

        finalColor = litColor;
    }

    // Output alpha. OPAQUE and MASK draw at alpha 1 per the glTF spec (MASK's
    // cutout is the discard above). BLEND keeps the texture alpha, and a
    // transmissive surface lets the backbuffer show through.
    float outAlpha = 1.0;
    if (m.flags.x == 2)
        outAlpha = baseColor.a;
    if (transmission > 0.0)
        outAlpha *= (1.0 - transmission);

    output.Color = float4(finalColor, outAlpha);
    output.EntityID = pc.entityID;

    return output;
}
