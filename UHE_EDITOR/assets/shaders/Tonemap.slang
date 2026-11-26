// Tonemap: an HDR scene texture to the display.
//
// Drawn as one fullscreen triangle generated from SV_VertexID (no vertex
// buffer). The HDR texture arrives by its bindless slot. ACES filmic (Narkowicz
// fit) maps scene-referred colour into [0, 1]; the output is linear, and an
// sRGB render target encodes it.
//
// An optional bloom image (Bloom.slang's chain, at half resolution) is mixed
// in before exposure: it is scene-referred light too, so it goes through the
// same curve as the rest of the image instead of being added on top of it.
//
// FXAA (fxaa = 1) smooths geometry edges in the same pass: every tap it takes
// is tonemapped first, because edges are judged on what the display shows,
// not on scene-referred values. Four diagonal taps decide whether a pixel is
// on an edge and which way the edge runs; two or four more along it blend the
// stair step away (FXAA 3.x "console" quality).

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

struct PushConstants
{
    uint hdrTexture;      // bindless slot of the HDR scene colour
    float exposure;       // linear multiplier applied before the curve
    int bloomTexture;     // bindless slot of the bloomed image; -1 = no bloom
    float bloomIntensity; // share of the bloomed image in the result
    int flipY;            // 1: read the input upside down (a game drawing into the backbuffer)
    int fxaa;             // 1: anti-alias edges
    int debugView;        // 0 the image; 1 ambient occlusion; 2 depth as distance; 3 bloom alone
    int debugTexture;     // bindless slot of the buffer shown (1, 2)
    float depthA;         // 2: clip.z = depthA * z + depthB, the projection's terms
    float depthB;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc;
[[vk::binding(1, 0)]] Sampler2D u_Textures[10000];

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID)
{
    // Vertices (0,0), (2,0), (0,2) in uv cover the screen with one triangle.
    // Vulkan's clip space has y down, so uv (0,0) is the top-left texel.
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    if (pc.flipY != 0)
        output.uv.y = 1.0 - output.uv.y;
    return output;
}

float3 ACESFilm(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

/// The displayed colour at `uv`: scene and bloom mixed, exposed, tonemapped.
float3 Display(float2 uv)
{
    float3 hdr = u_Textures[NonUniformResourceIndex(pc.hdrTexture)].SampleLevel(uv, 0.0).rgb;
    if (pc.bloomTexture >= 0)
    {
        // Same uv: the linear sampler upsamples the half-resolution bloom.
        const float3 bloom = u_Textures[NonUniformResourceIndex(uint(pc.bloomTexture))].SampleLevel(uv, 0.0).rgb;
        hdr = lerp(hdr, bloom, pc.bloomIntensity);
    }
    return ACESFilm(hdr * pc.exposure);
}

float Luma(float3 c)
{
    // On the display curve, not linear light: FXAA's thresholds are perceptual.
    return sqrt(dot(c, float3(0.299, 0.587, 0.114)));
}

float3 Fxaa(float2 uv, float3 centre)
{
    uint width, height;
    u_Textures[NonUniformResourceIndex(pc.hdrTexture)].GetDimensions(width, height);
    const float2 texel = 1.0 / float2(width, height);

    const float lumaM = Luma(centre);
    const float lumaNW = Luma(Display(uv + float2(-1.0, -1.0) * texel));
    const float lumaNE = Luma(Display(uv + float2(1.0, -1.0) * texel));
    const float lumaSW = Luma(Display(uv + float2(-1.0, 1.0) * texel));
    const float lumaSE = Luma(Display(uv + float2(1.0, 1.0) * texel));
    const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    // Flat or nearly flat: no edge to smooth.
    if (lumaMax - lumaMin < max(0.0312, lumaMax * 0.125))
        return centre;

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = (lumaNW + lumaSW) - (lumaNE + lumaSE);
    const float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.125), 1.0 / 128.0);
    const float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, -8.0, 8.0) * texel;

    const float3 near = 0.5 * (Display(uv + dir * (1.0 / 3.0 - 0.5)) + Display(uv + dir * (2.0 / 3.0 - 0.5)));
    const float3 wide = near * 0.5 + 0.25 * (Display(uv - dir * 0.5) + Display(uv + dir * 0.5));
    // The wider blend reached past the edge's own range: keep the narrow one.
    const float lumaWide = Luma(wide);
    return (lumaWide < lumaMin || lumaWide > lumaMax) ? near : wide;
}

/// An intermediate buffer, for the editor's View > Buffer.
float3 DebugBuffer(float2 uv)
{
    if (pc.debugView == 3)
    {
        if (pc.bloomTexture < 0)
            return float3(0.0, 0.0, 0.0);
        const float3 bloom = u_Textures[NonUniformResourceIndex(uint(pc.bloomTexture))].SampleLevel(uv, 0.0).rgb;
        return ACESFilm(bloom * pc.exposure);
    }
    if (pc.debugTexture < 0)
        return float3(1.0, 0.0, 1.0); // the buffer does not exist this frame (feature off)
    const float value = u_Textures[NonUniformResourceIndex(uint(pc.debugTexture))].SampleLevel(uv, 0.0).r;
    if (pc.debugView == 1)
        return float3(value, value, value);
    // Depth: metres from the camera, 0 white to 100 m black, the far plane as sky blue.
    if (value >= 0.999999)
        return float3(0.35, 0.45, 0.6);
    const float metres = pc.depthB / (value + pc.depthA);
    const float shade = 1.0 - saturate(metres / 100.0);
    return float3(shade, shade, shade) * shade;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    if (pc.debugView != 0)
        return float4(DebugBuffer(input.uv), 1.0);
    const float3 colour = Display(input.uv);
    return float4(pc.fxaa != 0 ? Fxaa(input.uv, colour) : colour, 1.0);
}
