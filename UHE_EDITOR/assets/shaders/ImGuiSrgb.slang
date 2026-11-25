// ImGui drawn into an sRGB swapchain.
//
// ImGui and the editor theme give colours in sRGB, the way a colour picker
// shows them. The swapchain is *_SRGB, so the hardware encodes whatever the
// fragment shader writes as if it were linear: ImGui's own shader writes the
// sRGB colour unchanged, it is encoded a second time, and the theme's 0.095
// background reached the screen as 0.34 grey. This pair replaces ImGui's
// built-in shaders (same interface as imgui_impl_vulkan.cpp) and decodes the
// vertex colour to linear first, in float, so close dark shades stay apart.
// Textures need nothing: the sampler decodes an sRGB texture already.

struct VSInput
{
    [[vk::location(0)]] float2 position;
    [[vk::location(1)]] float2 uv;
    [[vk::location(2)]] float4 color; // R8G8B8A8_UNORM, sRGB-encoded
};

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color;
    [[vk::location(1)]] float2 uv;
};

struct PushConstants
{
    float2 scale;
    float2 translate;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc;
[[vk::binding(0, 0)]] Sampler2D sTexture;

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.color = input.color;
    output.uv = input.uv;
    output.position = float4(input.position * pc.scale + pc.translate, 0.0, 1.0);
    return output;
}

float3 SrgbToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target0
{
    const float4 color = float4(SrgbToLinear(input.color.rgb), input.color.a);
    return color * sTexture.Sample(input.uv);
}
