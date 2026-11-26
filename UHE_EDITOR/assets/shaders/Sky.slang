// The sky: drawn after the opaque scene into its HDR image, on the pixels no
// geometry covered (the depth is still the cleared far plane there).
//
// Procedural (mode 1): a gradient from the zenith to the horizon, a ground
// colour below it, the sun's glow and disk. Everything follows the sun: the
// horizon warms as it sets, the sky darkens to night once it is down, and the
// disk is drawn in HDR at many times the light's colour, so bloom picks it up.
//
// Image (mode 2): an equirectangular panorama, read by direction. A .hdr
// panorama arrives as half floats and keeps its real brightness.
//
// Depth is read with Load: depth formats need not support filtering.

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 ndc;
};

// MUST match SkyPushConstants in SkyRenderer.cpp.
struct PushConstants
{
    float4x4 inverseViewProjection;
    float4 cameraPosition; // xyz
    float4 sunDirection;   // xyz: towards the sun; w: 1 when the scene has a sun
    float4 sunColor;       // rgb: the sun light's linear colour x intensity
    float4 zenith;         // rgb: linear; a: sky intensity
    float4 horizon;        // rgb: linear; a: cos of the sun disk's angular radius
    float4 ground;         // rgb: linear; a: sun disk brightness
    int depthTexture;      // bindless slot of the scene depth
    int mode;              // 1: procedural, 2: image
    int imageTexture;      // bindless slot of the panorama, -1 without one
    float imageRotation;   // radians about +Y
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc;
[[vk::binding(1, 0)]] Sampler2D u_Textures[10000];

static const float PI = 3.14159265;

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    const float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.ndc = uv * 2.0 - 1.0;
    output.position = float4(output.ndc, 0.0, 1.0);
    return output;
}

/// Light arriving from direction `d` (unit length), procedural sky.
float3 ProceduralSky(float3 d)
{
    const bool hasSun = pc.sunDirection.w > 0.5;
    const float3 toSun = hasSun ? normalize(pc.sunDirection.xyz) : normalize(float3(0.3, 0.6, 0.2));
    const float elevation = toSun.y;

    // 1 by day, a dark blue night once the sun is well below the horizon.
    const float day = smoothstep(-0.28, 0.12, elevation);
    // Strongest while the sun is low, gone at noon and at night.
    const float sunset = (1.0 - smoothstep(0.02, 0.42, elevation)) * smoothstep(-0.30, -0.02, elevation);

    const float up = d.y;
    const float horizonBlend = pow(1.0 - saturate(up), 5.0);
    float3 sky = lerp(pc.zenith.rgb, pc.horizon.rgb, horizonBlend);

    // Warm the horizon, most of all on the side the sun is on.
    const float2 flatDir = normalize(d.xz + 1e-5);
    const float2 flatSun = normalize(toSun.xz + 1e-5);
    const float facing = dot(flatDir, flatSun) * 0.5 + 0.5;
    const float3 sunsetColor = float3(1.0, 0.32, 0.07);
    sky = lerp(sky, sky * 0.25 + sunsetColor, sunset * horizonBlend * (0.3 + 0.7 * facing * facing));

    const float3 night = pc.zenith.rgb * float3(0.02, 0.03, 0.06);
    sky = lerp(night, sky, day);

    // Below the horizon: distant ground is hazed into the horizon colour, and
    // only reaches the ground colour looking well down. A scene's ground mesh
    // ends short of the true horizon, and a hard ground colour there drew a
    // dark band between the two.
    // `sky` here is already the (sunset-tinted) horizon: up is clamped to 0 above.
    const float3 haze = sky;
    const float3 groundColor = lerp(haze, pc.ground.rgb * lerp(0.04, 1.0, day), smoothstep(0.0, -0.25, up));
    float3 color = up >= 0.0 ? sky : groundColor;

    if (hasSun)
    {
        const float3 sunColor = pc.sunColor.rgb;
        const float cosAngle = dot(d, toSun);
        const float aboveHorizon = smoothstep(-0.02, 0.01, up);
        // Glow: a wide faint halo and a tight bright one (forward scattering).
        const float glow = pow(saturate(cosAngle), 8.0) * 0.06 + pow(saturate(cosAngle), 180.0) * 0.6;
        color += sunColor * glow * day * aboveHorizon;
        // The disk, with a soft edge one fifth of its radius wide.
        const float cosRadius = pc.horizon.a;
        const float edge = (1.0 - cosRadius) * 0.4;
        const float disk = smoothstep(cosRadius - edge, cosRadius + edge, cosAngle);
        color += sunColor * pc.ground.a * disk * aboveHorizon;
    }
    return color;
}

/// Light arriving from direction `d`, equirectangular panorama.
float3 ImageSky(float3 d)
{
    if (pc.imageTexture < 0)
        return pc.horizon.rgb;
    const float azimuth = atan2(d.x, -d.z) + pc.imageRotation;
    const float u = frac(azimuth / (2.0 * PI) + 0.5);
    // Images are flipped on load: the top row (the zenith) is at v = 1.
    const float v = 1.0 - acos(clamp(d.y, -1.0, 1.0)) / PI;
    // Level 0: the u seam would otherwise pick the smallest mip in a line down the sky.
    return u_Textures[NonUniformResourceIndex(pc.imageTexture)].SampleLevel(float2(u, v), 0.0).rgb;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    int2 size;
    int levels;
    u_Textures[NonUniformResourceIndex(pc.depthTexture)].GetDimensions(0, size.x, size.y, levels);
    const int2 pixel = clamp(int2(input.position.xy), int2(0, 0), size - 1);
    const float depth = u_Textures[NonUniformResourceIndex(pc.depthTexture)].Load(int3(pixel, 0)).r;
    if (depth < 0.999999)
        discard;

    const float4 farPoint = mul(pc.inverseViewProjection, float4(input.ndc, 1.0, 1.0));
    const float4 nearPoint = mul(pc.inverseViewProjection, float4(input.ndc, 0.0, 1.0));
    // From the near to the far plane, not from the camera position: correct for
    // orthographic cameras too, whose rays are parallel.
    const float3 direction = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);

    const float3 color = pc.mode == 2 ? ImageSky(direction) : ProceduralSky(direction);
    return float4(color * pc.zenith.a, 1.0);
}
