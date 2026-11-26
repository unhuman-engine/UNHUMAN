// Game UI over the finished frame: rounded boxes (with a border and an
// optional image) and MSDF text, from one vertex stream, straight alpha.
//
// Positions arrive in UI pixels (origin top left, y down). A box is shaded by
// the signed distance to its rounded rectangle, so its corners and border are
// antialiased at any size; text by the median of the font atlas's three
// distance channels, as Text.slang does.

struct VSInput
{
    [[vk::location(0)]] float2 position;    // UI pixels
    [[vk::location(1)]] float2 uv;
    [[vk::location(2)]] float4 color;       // linear, straight alpha
    [[vk::location(3)]] float4 rect;        // box: xy pixel offset from the box centre, zw half size
    [[vk::location(4)]] float4 params;      // box: x corner radius, y border width; text: x unit range; z mode; w texture
    [[vk::location(5)]] float4 borderColor; // linear, straight alpha
};

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
    [[vk::location(1)]] float4 color;
    [[vk::location(2)]] float4 rect;
    [[vk::location(3)]] nointerpolation float4 params;
    [[vk::location(4)]] float4 borderColor;
};

// MUST match UIPushConstants in UIRenderer.cpp.
struct PushConstants
{
    float2 screenSize; // pixels
    float flipY;       // 1: the target is shown upside down (the editor's viewport image)
    float padding;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc;
[[vk::binding(1, 0)]] Sampler2D u_Textures[10000];

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float2 ndc = input.position / pc.screenSize * 2.0 - 1.0;
    if (pc.flipY > 0.5)
        ndc.y = -ndc.y;
    output.position = float4(ndc, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    output.rect = input.rect;
    output.params = input.params;
    output.borderColor = input.borderColor;
    return output;
}

/// Signed distance from `p` (relative to the centre) to a rounded box of half size `b`, corner radius `r`.
float RoundedBox(float2 p, float2 b, float r)
{
    const float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float Median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const int mode = int(input.params.z + 0.5);
    const int texture = int(round(input.params.w));

    if (mode == 1)
    {
        const float3 msdf = u_Textures[NonUniformResourceIndex(texture)].Sample(input.uv).rgb;
        const float2 unitRange = float2(input.params.x, input.params.x);
        const float screenPxRange = max(0.5 * dot(unitRange, 1.0 / fwidth(input.uv)), 1.0);
        const float alpha = saturate((Median(msdf.r, msdf.g, msdf.b) - 0.5) * screenPxRange + 0.5);
        if (alpha < 0.005)
            discard;
        return float4(input.color.rgb, input.color.a * alpha);
    }

    const float2 halfSize = input.rect.zw;
    const float radius = min(input.params.x, min(halfSize.x, halfSize.y));
    const float distance = RoundedBox(input.rect.xy, halfSize, radius);
    const float coverage = saturate(0.5 - distance);
    if (coverage <= 0.0)
        discard;

    float4 fill = input.color;
    if (texture >= 0)
        fill *= u_Textures[NonUniformResourceIndex(texture)].Sample(input.uv);
    const float border = input.params.y;
    if (border > 0.0)
    {
        // 1 inside the border's inner edge, 0 on the border.
        const float inner = saturate(0.5 - (distance + border));
        fill = lerp(input.borderColor, fill, inner);
    }
    return float4(fill.rgb, fill.a * coverage);
}
