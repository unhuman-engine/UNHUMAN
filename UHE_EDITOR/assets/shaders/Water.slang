#pragma warning(disable: 41012)
// Water surfaces (WaterComponent), drawn after the opaque scene.
//
// Mode 0 copies the opaque scene into a new HDR image with a fullscreen
// triangle (the no_entity_id variant: that pass has no entity target); mode 1
// then draws each water surface on top and writes its entity ID for picking. The
// opaque colour and depth are read as textures, so the water can refract what
// is below it, measure how deep it is there, and hide itself behind geometry
// (the depth test is done here: the depth buffer is an input, not an
// attachment).
//
// Waves are four Gerstner waves (GPU Gems, ch. 1) in world space, travelling
// around one main direction; the normal is their analytic derivative plus two
// small ripples, evaluated per pixel. Colour: Fresnel between the refracted
// bottom, tinted and faded toward the deep colour by the water's thickness,
// and the sky reflected about the normal; a sun highlight; foam where the
// water is shallow and on sharp crests.

struct VSOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float2 flatXZ; // the undisplaced position: where the waves are evaluated
    [[vk::location(2)]] float crest;   // how high this vertex rides, 0..1
};

// MUST match WaterPushConstants in WaterRenderer.cpp.
struct PushConstants
{
    float4x4 viewProj;
    float4x4 model;
    float4 cameraPos;    // xyz: eye, w: time (s)
    float4 shallow;      // rgb: shallow tint (linear), a: clarity (m)
    float4 deep;         // rgb: deep colour (linear), a: foam
    float4 wave;         // x: height, y: length, z: speed, w: choppiness
    float4 waveDir;      // xy: main direction on x/z, z: grid cells per side, w: mode (0 copy, 1 water)
    float2 size;         // extent on local x/z
    int entityID;        // picking; an int, a float would lose entt handles above 2^24
    float padding;
    float4 projection;   // depth to view z: P[2][2], P[3][2], P[2][3], P[3][3]
    int frameData;       // SceneFrameData buffer (-1 none)
    int lightBuffer;     // light buffer
    int sceneColor;      // opaque HDR colour
    int sceneDepth;      // opaque depth
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc;
[[vk::binding(1, 0)]] Sampler2D u_Textures[10000];

struct LightData {
    float4 type_radius_cones;
    float4 position_or_direction;
    float4 direction_pad;
    float4 color_intensity;
    float4 area_right;        // area light: the rectangle's width axis
};

// MUST match Renderer3D::SceneFrameData (and FrameData in Basic3D.slang).
struct FrameData {
    float4x4 cascadeViewProj[4];
    float4 cascadeSplits;
    float4 cascadeRects[4];
    float4 cascadeTexel;
    float4 shadow;
    float4 shadowBias;
    float4 cameraForward;
    float4 ambientSky;
    float4 ambientGround;
    float4 screen;
    float4 wind;
    float4 flags;
    float4 sky;
    float4 localShadow;       // x: local atlas texture (-1 none), y: tiles, z: tile buffer
    float4 localShadowBias;   // x: toward the light, y: along the normal (texels)
};
[[vk::binding(0, 0)]] StructuredBuffer<LightData> u_LightBuffers[10000];
[[vk::binding(0, 0)]] StructuredBuffer<FrameData> u_FrameBuffers[10000];

static const float kPi = 3.14159265;
static const float kGravity = 9.81;

/// One Gerstner wave: displacement of the surface point at `xz`, and the partial derivatives for its normal.
void Gerstner(float2 xz, float2 direction, float wavelength, float amplitude, float steepness, float speed, float t,
              inout float3 offset, inout float3 tangentSum)
{
    const float k = 2.0 * kPi / wavelength;
    const float w = sqrt(kGravity * k) * speed; // deep-water dispersion: long waves are faster
    const float phase = k * dot(direction, xz) - w * t;
    const float c = cos(phase);
    const float s = sin(phase);
    offset += float3(steepness * amplitude * direction.x * c, amplitude * s, steepness * amplitude * direction.y * c);
    // x: d(height)/dx, z: d(height)/dz, y: the crest pinching (sharpens the normal at the top).
    tangentSum += float3(direction.x * k * amplitude * c, steepness * k * amplitude * s, direction.y * k * amplitude * c);
}

/// The four waves: one main wave and three shorter ones fanned around its direction.
void Waves(float2 xz, out float3 offset, out float3 normal)
{
    const float t = pc.cameraPos.w;
    const float2 d = normalize(pc.waveDir.xy + float2(1e-5, 0.0));
    const float2 side = float2(-d.y, d.x);
    const float height = pc.wave.x;
    const float length = max(pc.wave.y, 0.1);
    const float speed = pc.wave.z;
    // Q per wave is kept below 1 / (k A count), or crests loop over themselves.
    const float chop = saturate(pc.wave.w);

    offset = float3(0.0, 0.0, 0.0);
    float3 sum = float3(0.0, 0.0, 0.0);
    const float lengths[4] = { 1.0, 0.61, 0.37, 0.23 };
    const float amplitudes[4] = { 1.0, 0.55, 0.30, 0.16 };
    const float2 dirs[4] = { d, normalize(d + side * 0.45), normalize(d - side * 0.6), normalize(d + side * 1.1) };
    for (int i = 0; i < 4; ++i)
    {
        const float L = length * lengths[i];
        const float A = height * amplitudes[i];
        const float k = 2.0 * kPi / L;
        Gerstner(xz, dirs[i], L, A, chop / max(k * A * 4.0, 1e-4), speed, t, offset, sum);
    }
    normal = normalize(float3(-sum.x, 1.0 - sum.y, -sum.z));
}

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    if (pc.waveDir.w < 0.5)
    {
        // Mode 0: the fullscreen copy of the opaque scene.
        const float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
        output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
        output.worldPos = float3(uv, 0.0);
        output.flatXZ = uv;
        output.crest = 0.0;
        return output;
    }

    // Mode 1: cell = vertexID / 6, two triangles per cell.
    const uint cells = max(uint(pc.waveDir.z), 1u);
    const uint cell = vertexID / 6u;
    const uint corner = vertexID % 6u;
    const uint2 cornerOffsets[6] = { uint2(0, 0), uint2(0, 1), uint2(1, 0), uint2(1, 0), uint2(0, 1), uint2(1, 1) };
    const uint2 grid = uint2(cell % cells, cell / cells) + cornerOffsets[corner];
    const float2 local = (float2(grid) / float(cells) - 0.5) * pc.size.xy;

    const float4 flat = mul(pc.model, float4(local.x, 0.0, local.y, 1.0));
    float3 offset;
    float3 normal;
    Waves(flat.xz, offset, normal);
    output.worldPos = flat.xyz + offset;
    output.flatXZ = flat.xz;
    output.crest = saturate(offset.y / max(pc.wave.x * 1.6, 1e-4) * 0.5 + 0.5);
    output.position = mul(pc.viewProj, float4(output.worldPos, 1.0));
    return output;
}

float3 SrgbToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

/// View-space distance in front of the camera for an NDC depth.
float ViewDistance(float depth)
{
    // clip.z = A z + B, clip.w = C z + D  =>  z = (B - d D) / (d C - A); the camera looks down -z.
    const float A = pc.projection.x;
    const float B = pc.projection.y;
    const float C = pc.projection.z;
    const float D = pc.projection.w;
    return -(B - depth * D) / (depth * C - A);
}

/// Cheap animated foam pattern: two scrolling interference grids.
float FoamPattern(float2 xz, float t)
{
    const float a = sin(xz.x * 2.3 + t * 0.9) * sin(xz.y * 2.9 - t * 0.7);
    const float b = sin((xz.x + xz.y) * 4.1 - t * 1.3) * sin((xz.x - xz.y) * 3.7 + t * 1.1);
    return saturate(a * 0.5 + b * 0.5 + 0.35);
}

/**
 * Screen-space reflection: marches the reflected ray through the opaque scene's
 * depth and returns the colour it hits, with how much to trust it in alpha.
 * Steps grow along the ray, so the far shore is reached in a few dozen steps;
 * a ray that leaves the screen or never passes behind a surface returns 0 and
 * the caller keeps the sky.
 */
float4 TraceReflection(float3 origin, float3 dir, Sampler2D color, Sampler2D depth, float2 size)
{
    float stepLength = 0.2;
    float3 previous = origin;
    for (int i = 0; i < 64; ++i)
    {
        const float3 p = previous + dir * stepLength;
        const float4 clip = mul(pc.viewProj, float4(p, 1.0));
        if (clip.w <= 0.0)
            break;
        const float2 ndc = clip.xy / clip.w;
        if (any(abs(ndc) > 1.0))
            break;
        const float sceneDistance = ViewDistance(depth.Load(int3(int2((ndc * 0.5 + 0.5) * size), 0)).r);
        // Behind a surface, but not so far behind that the ray passed under it.
        if (clip.w > sceneDistance && clip.w - sceneDistance < max(stepLength * 2.0, 0.5))
        {
            // Refine between the last point in front and this one.
            float3 lo = previous;
            float3 hi = p;
            for (int k = 0; k < 5; ++k)
            {
                const float3 mid = (lo + hi) * 0.5;
                const float4 c = mul(pc.viewProj, float4(mid, 1.0));
                const float d = ViewDistance(depth.Load(int3(int2((c.xy / c.w * 0.5 + 0.5) * size), 0)).r);
                if (c.w > d)
                    hi = mid;
                else
                    lo = mid;
            }
            const float4 c = mul(pc.viewProj, float4(hi, 1.0));
            const float2 uv = c.xy / c.w * 0.5 + 0.5;
            const float2 edge = saturate((1.0 - abs(c.xy / c.w)) * 6.0);
            // Four taps: a ray through a gap in thin foliage would otherwise sparkle pixel to pixel.
            const float2 texel = 1.0 / size;
            float3 hit = color.SampleLevel(uv + texel * float2(-1.0, -1.0), 0.0).rgb;
            hit += color.SampleLevel(uv + texel * float2(1.0, -1.0), 0.0).rgb;
            hit += color.SampleLevel(uv + texel * float2(-1.0, 1.0), 0.0).rgb;
            hit += color.SampleLevel(uv + texel * float2(1.0, 1.0), 0.0).rgb;
            return float4(hit * 0.25, edge.x * edge.y);
        }
        previous = p;
        stepLength *= 1.09;
    }
    return float4(0.0, 0.0, 0.0, 0.0);
}

struct FSOutput
{
    float4 color : SV_Target0;
#ifndef UHE_NO_ENTITY_ID
    int entityID : SV_Target1;
#endif
};

FSOutput Output(float3 color)
{
    FSOutput output;
    output.color = float4(color, 1.0);
#ifndef UHE_NO_ENTITY_ID
    output.entityID = pc.entityID;
#endif
    return output;
}

[shader("fragment")]
FSOutput fragmentMain(VSOutput input)
{
    Sampler2D sceneColor = u_Textures[NonUniformResourceIndex(pc.sceneColor)];
    Sampler2D sceneDepth = u_Textures[NonUniformResourceIndex(pc.sceneDepth)];
    uint width, height;
    sceneColor.GetDimensions(width, height);
    const int2 pixel = int2(input.position.xy);

    if (pc.waveDir.w < 0.5)
        return Output(sceneColor.Load(int3(pixel, 0)).rgb);

    // Behind the opaque scene: hidden.
    const float opaqueDepth = sceneDepth.Load(int3(pixel, 0)).r;
    if (input.position.z > opaqueDepth)
        discard;

    const float t = pc.cameraPos.w;
    float3 offset;
    float3 N;
    Waves(input.flatXZ, offset, N);
    // Small ripples for detail the vertex grid cannot carry, along directions
    // off the world axes so they do not line up into a visible grid.
    const float2 xz = input.flatXZ;
    const float r1 = dot(xz, float2(0.80, 0.60)) * 3.1 + t * 0.7;
    const float r2 = dot(xz, float2(-0.47, 0.88)) * 4.3 - t * 0.6;
    const float r3 = dot(xz, float2(0.26, -0.97)) * 6.7 + t * 0.9;
    N = normalize(N + float3(0.80, 0.0, 0.60) * cos(r1) * 0.035 + float3(-0.47, 0.0, 0.88) * cos(r2) * 0.03 +
                  float3(0.26, 0.0, -0.97) * cos(r3) * 0.02);

    // Far away the ripples are smaller than a pixel and only alias into sparkle: calm them with distance.
    const float viewDistance = length(pc.cameraPos.xyz - input.worldPos);
    N = normalize(lerp(N, float3(0.0, 1.0, 0.0), smoothstep(6.0, 45.0, viewDistance) * 0.9));

    const float3 V = normalize(pc.cameraPos.xyz - input.worldPos);
    const float NoV = saturate(dot(N, V));
    const float fresnel = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);

    // How much water lies between the surface and the bottom, along the view ray.
    const float surfaceDistance = ViewDistance(input.position.z);
    const float bottomDistance = ViewDistance(opaqueDepth);
    const float thickness = max(bottomDistance - surfaceDistance, 0.0);

    // Refraction: the bottom seen through the surface, shifted by its slope.
    const float2 screenSize = float2(width, height);
    const float2 uv = input.position.xy / screenSize;
    float2 refractedUV = uv + N.xz * 0.04 * saturate(thickness * 0.5);
    // A shifted sample that lands on something in front of the water would
    // pull it into the refraction; such pixels keep the straight sample.
    const float refractedDepth = sceneDepth.Load(int3(int2(refractedUV * screenSize), 0)).r;
    if (refractedDepth < input.position.z)
        refractedUV = uv;
    const float3 bottom = sceneColor.SampleLevel(refractedUV, 0.0).rgb;
    const float refractedThickness =
        max(ViewDistance(max(refractedDepth, input.position.z)) - surfaceDistance, 0.0);

    const float clarity = max(pc.shallow.a, 0.05);
    const float absorbed = 1.0 - exp(-max(refractedThickness, thickness) / clarity);
    const float3 underwater = lerp(bottom * pc.shallow.rgb, pc.deep.rgb, absorbed);

    // Sky above the horizon, a darker sea colour below it.
    float3 skyColor = float3(0.45, 0.6, 0.8);
    float3 ambient = float3(0.15, 0.18, 0.22);
    float3 sunDirection = normalize(float3(0.4, 0.8, 0.3));
    float3 sunColor = float3(1.0, 1.0, 1.0);
    if (pc.frameData >= 0)
    {
        const FrameData frame = u_FrameBuffers[pc.frameData][0];
        skyColor = frame.sky.rgb;
        ambient = frame.ambientSky.rgb;
        const int sun = int(frame.sky.w);
        if (sun >= 0 && pc.lightBuffer >= 0)
        {
            const LightData light = u_LightBuffers[pc.lightBuffer][sun];
            sunDirection = normalize(-light.position_or_direction.xyz);
            sunColor = light.color_intensity.rgb * light.color_intensity.w;
        }
    }
    const float3 R = reflect(-V, N);
    float3 reflection = lerp(pc.deep.rgb * 0.5 + ambient, skyColor, smoothstep(-0.05, 0.25, R.y));
    // What stands on the shore (trees, rocks) is mirrored where the screen holds it; the sky elsewhere.
    const float4 mirrored = TraceReflection(input.worldPos, R, sceneColor, sceneDepth, screenSize);
    reflection = lerp(reflection, mirrored.rgb, mirrored.a);

    float3 color = lerp(underwater, reflection, fresnel);
    // The deep colour still catches some sky light from above.
    color += pc.deep.rgb * ambient * (1.0 - fresnel);

    // Sun glint: a sharp highlight with a soft halo.
    const float3 H = normalize(V + sunDirection);
    const float NoH = saturate(dot(N, H));
    const float NoL = saturate(dot(N, sunDirection));
    // The sharp glint fades with distance for the same reason the ripples do: sub-pixel sparkle.
    const float glint = pow(NoH, 600.0) * 6.0 * (1.0 - smoothstep(10.0, 60.0, viewDistance) * 0.9);
    color += sunColor * (glint + pow(NoH, 60.0) * 0.12) * NoL;

    // Foam: along the shore, where the water is thin, and on the sharpest crests.
    const float foamAmount = saturate(pc.deep.a);
    const float shore = 1.0 - smoothstep(0.0, 0.4, thickness);
    const float crest = smoothstep(0.85, 1.0, input.crest) * saturate(pc.wave.w) * 0.6;
    const float pattern = FoamPattern(input.flatXZ, t);
    const float foam = saturate((shore + crest) * foamAmount) * pattern * pattern;
    color = lerp(color, float3(0.9, 0.93, 0.95) * (ambient + sunColor * NoL * 0.6), foam);

    // A soft edge where the surface meets geometry, instead of a hard line.
    const float edge = smoothstep(0.0, 0.08, thickness);
    color = lerp(sceneColor.Load(int3(pixel, 0)).rgb, color, edge);
    return Output(color);
}
