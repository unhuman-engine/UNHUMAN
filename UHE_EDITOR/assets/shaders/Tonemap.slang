// Tonemap: an HDR scene texture to the display.
//
// Drawn as one fullscreen triangle generated from SV_VertexID (no vertex
// buffer). The HDR texture arrives by its bindless slot. ACES filmic (Narkowicz
// fit) maps scene-referred colour into [0, 1]; the output is linear, and an
// sRGB render target encodes it.

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

struct PushConstants
{
    uint hdrTexture; // bindless slot of the HDR scene colour
    float exposure;  // linear multiplier applied before the curve
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
    const float3 hdr = u_Textures[NonUniformResourceIndex(pc.hdrTexture)].Sample(input.uv).rgb;
    return float4(ACESFilm(hdr * pc.exposure), 1.0);
}
