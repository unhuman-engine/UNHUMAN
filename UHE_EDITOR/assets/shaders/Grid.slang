// Editor ground grid: the y = 0 plane, drawn over the scene like the floor of
// Blender's and Godot's viewports.
//
// One fullscreen quad; each pixel casts its view ray onto the plane. Lines come
// in three levels (1, 10 and 100 units), and each level fades in as its cells
// grow past a few pixels on screen and out as they shrink, so the grid neither
// turns to moire far away nor to a few lonely lines up close. Lines are
// antialiased in screen space at a fixed pixel width, and the whole plane fades
// with distance (scaled by the camera's height) and at grazing angles. The X
// axis is red and the Z axis blue; Y is up.
//
// Colours are written in linear space: the target is sRGB and encodes them.

struct VSInput
{
    uint vertexID : SV_VertexID;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 nearPoint;
    float3 farPoint;
};

// MUST match GridPushConstants in Renderer3D.cpp.
struct PushConstants
{
    float4x4 viewProj;
    float4x4 inverseViewProj;
    float4 cameraPos; // xyz: eye position
    float4 options;   // x: opacity 0..1, y: axis lines on (1) / off (0)
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> u_PushConstants;

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;

    float2 positions[6] = {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0,  1.0)
    };
    const float2 pos = positions[input.vertexID];

    // The pixel's view ray, as the points it passes on the near and far planes.
    const float4 nearPoint = mul(u_PushConstants.inverseViewProj, float4(pos, 0.0, 1.0));
    const float4 farPoint = mul(u_PushConstants.inverseViewProj, float4(pos, 1.0, 1.0));
    output.nearPoint = nearPoint.xyz / nearPoint.w;
    output.farPoint = farPoint.xyz / farPoint.w;
    output.position = float4(pos, 0.9999, 1.0);
    return output;
}

struct FSOutput
{
    float4 color : SV_Target0;
#ifndef UHE_NO_ENTITY_ID
    int entityID : SV_Target1;
#endif
    float depth : SV_Depth;
};

float3 SrgbToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

/// Coverage of the lines of a grid with `spacing`-wide cells, `widthPx` pixels thick.
float LineCoverage(float2 coord, float2 unitsPerPixel, float spacing, float widthPx)
{
    const float2 pixelsPerUnit = spacing / max(unitsPerPixel, 1e-6);
    // Distance to the nearest line, in pixels.
    const float2 distance = abs(frac(coord / spacing + 0.5) - 0.5) * pixelsPerUnit;
    const float2 coverage = saturate(widthPx * 0.5 + 0.5 - distance);
    return max(coverage.x, coverage.y);
}

/// How visible a level is: none while its cells are a few pixels wide, all once they are large.
float LevelFade(float2 unitsPerPixel, float spacing)
{
    const float cellPixels = spacing / max(max(unitsPerPixel.x, unitsPerPixel.y), 1e-6);
    return smoothstep(5.0, 22.0, cellPixels);
}

[shader("fragment")]
FSOutput fragmentMain(VSOutput input)
{
    FSOutput output;

    // Where the view ray meets y = 0; a ray that never does draws nothing.
    const float3 ray = input.farPoint - input.nearPoint;
    if (abs(ray.y) < 1e-8)
        discard;
    const float t = -input.nearPoint.y / ray.y;
    if (t < 0.0)
        discard;
    const float3 position = input.nearPoint + t * ray;

    const float2 coord = position.xz;
    const float2 unitsPerPixel = fwidth(coord);

    // Three levels, each brighter and a little thicker than the one below.
    // Kept quiet, as in Blender: the grid is a reference, not part of the scene.
    const float minor = LineCoverage(coord, unitsPerPixel, 1.0, 1.0) * LevelFade(unitsPerPixel, 1.0) * 0.16;
    const float major = LineCoverage(coord, unitsPerPixel, 10.0, 1.2) * LevelFade(unitsPerPixel, 10.0) * 0.32;
    const float huge = LineCoverage(coord, unitsPerPixel, 100.0, 1.4) * LevelFade(unitsPerPixel, 100.0) * 0.42;
    float alpha = max(minor, max(major, huge));
    float3 color = SrgbToLinear(float3(0.62, 0.63, 0.66));

    if (u_PushConstants.options.y > 0.5)
    {
        // The X axis runs along z = 0, the Z axis along x = 0; two pixels wide.
        const float xAxis = saturate(1.5 - abs(position.z) / max(unitsPerPixel.y, 1e-6));
        const float zAxis = saturate(1.5 - abs(position.x) / max(unitsPerPixel.x, 1e-6));
        if (xAxis > 0.0)
        {
            color = lerp(color, SrgbToLinear(float3(0.91, 0.30, 0.34)), xAxis);
            alpha = max(alpha, xAxis * 0.8);
        }
        if (zAxis > 0.0)
        {
            color = lerp(color, SrgbToLinear(float3(0.27, 0.53, 0.97)), zAxis);
            alpha = max(alpha, zAxis * 0.8);
        }
    }

    // Fade out with distance, further the higher the camera, and where the
    // plane is seen edge-on (where lines would alias into noise).
    const float3 eye = u_PushConstants.cameraPos.xyz;
    const float radius = clamp(abs(eye.y) * 25.0, 30.0, 3000.0);
    const float distanceFade = 1.0 - smoothstep(radius * 0.1, radius, length(position - eye));
    const float3 viewDirection = normalize(position - eye);
    const float angleFade = smoothstep(0.0, 0.2, abs(viewDirection.y));
    alpha *= distanceFade * angleFade * saturate(u_PushConstants.options.x);

    if (alpha < 0.003)
        discard;

    output.color = float4(color, alpha);
#ifndef UHE_NO_ENTITY_ID
    output.entityID = -1;
#endif
    const float4 clip = mul(u_PushConstants.viewProj, float4(position, 1.0));
    output.depth = clip.z / clip.w;
    return output;
}
