// Screen-space ambient occlusion from the depth prepass.
//
// Mode 0 (occlusion): each pixel rebuilds its view-space position from depth
// and its normal from the neighbouring depths, then tests sixteen points of a
// hemisphere around that normal against the depth buffer. A point that lies
// behind what the camera sees there is occluded; a range check stops a wall
// far behind from darkening an edge in front of it. The kernel is rotated per
// pixel, which trades banding for noise that mode 1 removes.
//
// Mode 1 (blur): a 5x5 blur weighted by depth, so occlusion does not bleed
// across silhouettes.
//
// Both run at half resolution: occlusion is low-frequency, and a quarter of
// the pixels is a quarter of the cost. Each half-resolution pixel reads the
// full-resolution depth at its top-left full pixel; the colour pass
// upsamples the result bilinearly.
//
// Depth is read with Load, never filtered: depth formats need not support
// linear filtering.

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

// MUST match SSAOPushConstants in SSAO.cpp.
struct PushConstants
{
    float4x4 projection;
    float4x4 inverseProjection;
    int depthTexture;  // bindless slot of the scene depth
    int sourceTexture; // mode 1: the raw occlusion to blur
    int mode;          // 0: occlusion, 1: blur
    float radius;      // world units
    float bias;        // view-space depth bias
    float intensity;   // exponent on the result
    float2 padding;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc;
[[vk::binding(1, 0)]] Sampler2D u_Textures[10000];

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

float LoadDepth(int2 pixel, int2 size)
{
    return u_Textures[NonUniformResourceIndex(pc.depthTexture)].Load(int3(clamp(pixel, int2(0, 0), size - 1), 0)).r;
}

/// View-space position of the pixel centre at `pixel` with depth `depth`.
float3 ViewPosition(int2 pixel, int2 size, float depth)
{
    const float2 uv = (float2(pixel) + 0.5) / float2(size);
    const float4 p = mul(pc.inverseProjection, float4(uv * 2.0 - 1.0, depth, 1.0));
    return p.xyz / p.w;
}

/// View-space z of an NDC depth, from the projection's z and w rows: a few
/// multiplies instead of a full unprojection, for the taps that need only z.
float ViewZ(float depth)
{
    // clip.z = A z + B and clip.w = C z + D, so d = clip.z / clip.w gives z.
    const float A = pc.projection[2][2];
    const float B = pc.projection[2][3];
    const float C = pc.projection[3][2];
    const float D = pc.projection[3][3];
    return (B - depth * D) / (depth * C - A);
}

/// Pseudo-random hemisphere direction i of 16: z up, denser near the centre.
float3 KernelSample(int i)
{
    // Golden-angle spiral over the hemisphere: an even spread without a table.
    const float t = (float(i) + 0.5) / 16.0;
    const float phi = float(i) * 2.39996323;
    const float cosTheta = sqrt(1.0 - t);
    const float sinTheta = sqrt(t);
    float3 v = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    // Length grows with i: most samples close to the surface, where contact shadows are.
    const float scale = lerp(0.1, 1.0, t * t);
    return v * scale;
}

float Occlusion(int2 pixel, int2 size)
{
    const float depth = LoadDepth(pixel, size);
    if (depth >= 1.0)
        return 1.0; // sky: nothing to occlude

    const float3 P = ViewPosition(pixel, size, depth);

    // Normal from the neighbour on each axis whose depth is closest to this
    // one, so a pixel on a silhouette takes its slope from its own surface.
    const float3 left = ViewPosition(pixel - int2(1, 0), size, LoadDepth(pixel - int2(1, 0), size));
    const float3 right = ViewPosition(pixel + int2(1, 0), size, LoadDepth(pixel + int2(1, 0), size));
    const float3 up = ViewPosition(pixel - int2(0, 1), size, LoadDepth(pixel - int2(0, 1), size));
    const float3 down = ViewPosition(pixel + int2(0, 1), size, LoadDepth(pixel + int2(0, 1), size));
    const float3 dx = abs(right.z - P.z) < abs(P.z - left.z) ? right - P : P - left;
    const float3 dy = abs(down.z - P.z) < abs(P.z - up.z) ? down - P : P - up;
    float3 N = normalize(cross(dy, dx));
    if (dot(N, -P) < 0.0)
        N = -N;

    // Interleaved gradient noise picks the kernel's rotation about N.
    const float noise = frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
    const float angle = noise * 6.2831853;
    const float3 random = float3(cos(angle), sin(angle), 0.0);
    const float3 T = normalize(random - N * dot(random, N));
    const float3 B = cross(N, T);
    const float3x3 TBN = float3x3(T, B, N); // rows

    float occluded = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        const float3 S = P + mul(KernelSample(i), TBN) * pc.radius;
        const float4 clip = mul(pc.projection, float4(S, 1.0));
        const float2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(uv < 0.0) || any(uv > 1.0))
            continue;
        const int2 samplePixel = int2(uv * float2(size));
        const float sceneZ = ViewZ(LoadDepth(samplePixel, size));
        // View space looks down -Z: a larger z is nearer the camera.
        const float range = smoothstep(0.0, 1.0, pc.radius / max(abs(P.z - sceneZ), 1e-4));
        occluded += (sceneZ >= S.z + pc.bias ? 1.0 : 0.0) * range;
    }
    return pow(saturate(1.0 - occluded / 16.0), pc.intensity);
}

float Blur(int2 pixel, int2 size, int2 depthSize)
{
    const float centreDepth = ViewZ(LoadDepth(pixel * 2, depthSize));
    Sampler2D source = u_Textures[NonUniformResourceIndex(pc.sourceTexture)];
    float sum = 0.0;
    float weights = 0.0;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            const int2 p = clamp(pixel + int2(x, y), int2(0, 0), size - 1);
            const float z = ViewZ(LoadDepth(p * 2, depthSize));
            // Neighbours on another surface (a depth jump) barely count.
            const float w = 1.0 / (1e-3 + abs(z - centreDepth) * 8.0);
            sum += source.Load(int3(p, 0)).r * w;
            weights += w;
        }
    }
    return sum / max(weights, 1e-5);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    uint width, height;
    u_Textures[NonUniformResourceIndex(pc.depthTexture)].GetDimensions(width, height);
    const int2 depthSize = int2(width, height);
    const int2 size = (depthSize + 1) / 2; // the half-resolution target
    const int2 pixel = int2(input.position.xy);
    const float value = pc.mode == 0 ? Occlusion(pixel * 2, depthSize) : Blur(pixel, size, depthSize);
    return float4(value, value, value, 1.0);
}
