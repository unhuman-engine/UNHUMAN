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

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    float3 hdr = u_Textures[NonUniformResourceIndex(pc.hdrTexture)].Sample(input.uv).rgb;
    if (pc.bloomTexture >= 0)
    {
        // Same uv: the linear sampler upsamples the half-resolution bloom.
        const float3 bloom = u_Textures[NonUniformResourceIndex(uint(pc.bloomTexture))].Sample(input.uv).rgb;
        hdr = lerp(hdr, bloom, pc.bloomIntensity);
    }
    return float4(ACESFilm(hdr * pc.exposure), 1.0);
}
