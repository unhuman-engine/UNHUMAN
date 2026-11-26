// Bloom: the downsample and upsample filters of physically based bloom.
//
// Jorge Jimenez, "Next Generation Post Processing in Call of Duty: Advanced
// Warfare" (SIGGRAPH 2014). There is no brightness threshold: the whole HDR
// image goes down a mip chain and back up, and the tonemap pass mixes the
// result in with a small weight, so bright areas bleed only in proportion to
// how bright they are.
//
// Entry points are fixed (vertexMain / fragmentMain), so one pipeline serves
// every pass and `mode` picks the filter. Drawn as one fullscreen triangle
// generated from SV_VertexID, like Tonemap.slang. Textures arrive by their
// bindless slot and are sampled linear, clamp to edge.

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv;
};

static const uint MODE_DOWNSAMPLE = 0;       // 13-tap filter
static const uint MODE_DOWNSAMPLE_KARIS = 1; // 13-tap filter, Karis average per group (first level only)
static const uint MODE_UPSAMPLE = 2;         // 3x3 tent over the smaller level, added to this level's downsample

struct PushConstants
{
    float2 srcTexelSize; // 1 / size of `srcTexture`, in uv
    uint srcTexture;     // bindless slot of the level being filtered
    int baseTexture;     // upsample: this level's downsample, added to the filtered image; -1 = none
    uint mode;           // MODE_*
    float filterRadius;  // upsample: tent radius in texels of `srcTexture`
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc;
[[vk::binding(1, 0)]] Sampler2D u_Textures[10000];

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID)
{
    // Vertices (0,0), (2,0), (0,2) in uv cover the screen with one triangle.
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

float3 Fetch(uint texture, float2 uv)
{
    return u_Textures[NonUniformResourceIndex(texture)].Sample(uv).rgb;
}

float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// A single over-bright texel (a specular sparkle) would otherwise survive the
// box filters as a flickering square; weighting each group by 1/(1+luma)
// pulls it towards its neighbours before it is spread over the whole chain.
float KarisWeight(float3 c)
{
    return 1.0 / (1.0 + Luma(c));
}

// The 13-tap filter: four overlapping 2x2 boxes around the centre plus one
// inner box, weighted 0.125 each and 0.5 for the inner one. Reading 13
// bilinear taps (36 texels) instead of one 2x2 box keeps the chain from
// aliasing and pulsing when the camera moves.
float3 Downsample(float2 uv, bool karis)
{
    const float2 t = pc.srcTexelSize;
    const uint src = pc.srcTexture;

    const float3 a = Fetch(src, uv + t * float2(-2.0, -2.0));
    const float3 b = Fetch(src, uv + t * float2( 0.0, -2.0));
    const float3 c = Fetch(src, uv + t * float2( 2.0, -2.0));
    const float3 d = Fetch(src, uv + t * float2(-2.0,  0.0));
    const float3 e = Fetch(src, uv);
    const float3 f = Fetch(src, uv + t * float2( 2.0,  0.0));
    const float3 g = Fetch(src, uv + t * float2(-2.0,  2.0));
    const float3 h = Fetch(src, uv + t * float2( 0.0,  2.0));
    const float3 i = Fetch(src, uv + t * float2( 2.0,  2.0));
    const float3 j = Fetch(src, uv + t * float2(-1.0, -1.0));
    const float3 k = Fetch(src, uv + t * float2( 1.0, -1.0));
    const float3 l = Fetch(src, uv + t * float2(-1.0,  1.0));
    const float3 m = Fetch(src, uv + t * float2( 1.0,  1.0));

    const float3 inner = (j + k + l + m) * 0.25;
    const float3 topLeft = (a + b + d + e) * 0.25;
    const float3 topRight = (b + c + e + f) * 0.25;
    const float3 bottomLeft = (d + e + g + h) * 0.25;
    const float3 bottomRight = (e + f + h + i) * 0.25;

    if (!karis)
        return inner * 0.5 + (topLeft + topRight + bottomLeft + bottomRight) * 0.125;

    // Normalized by the weights actually used, so the average stays an
    // average: dropping the division would darken the whole level.
    const float wInner = 0.5 * KarisWeight(inner);
    const float wTopLeft = 0.125 * KarisWeight(topLeft);
    const float wTopRight = 0.125 * KarisWeight(topRight);
    const float wBottomLeft = 0.125 * KarisWeight(bottomLeft);
    const float wBottomRight = 0.125 * KarisWeight(bottomRight);
    const float3 sum = inner * wInner + topLeft * wTopLeft + topRight * wTopRight + bottomLeft * wBottomLeft
                     + bottomRight * wBottomRight;
    return sum / (wInner + wTopLeft + wTopRight + wBottomLeft + wBottomRight);
}

// 3x3 tent (weights 1 2 1 / 2 4 2 / 1 2 1, over 16). Its footprint grows with
// `filterRadius`, which sets how far each level spreads before it is added
// to the next larger one.
float3 Upsample(float2 uv)
{
    const float2 r = pc.srcTexelSize * pc.filterRadius;
    const uint src = pc.srcTexture;

    float3 sum = Fetch(src, uv) * 4.0;
    sum += (Fetch(src, uv + float2(0.0, -r.y)) + Fetch(src, uv + float2(-r.x, 0.0))
            + Fetch(src, uv + float2(r.x, 0.0)) + Fetch(src, uv + float2(0.0, r.y)))
         * 2.0;
    sum += Fetch(src, uv + float2(-r.x, -r.y)) + Fetch(src, uv + float2(r.x, -r.y))
         + Fetch(src, uv + float2(-r.x, r.y)) + Fetch(src, uv + float2(r.x, r.y));
    return sum * (1.0 / 16.0);
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    float3 color;
    if (pc.mode == MODE_UPSAMPLE)
    {
        color = Upsample(input.uv);
        if (pc.baseTexture >= 0)
            color += Fetch(uint(pc.baseTexture), input.uv);
    }
    else
    {
        color = Downsample(input.uv, pc.mode == MODE_DOWNSAMPLE_KARIS);
    }
    // A NaN or negative from the scene would otherwise spread across the
    // whole chain as a black block. max() alone does not reliably drop NaN.
    if (any(isnan(color)))
        color = float3(0.0);
    color = max(color, float3(0.0));
    return float4(color, 1.0);
}
