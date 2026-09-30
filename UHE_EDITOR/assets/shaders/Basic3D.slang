#pragma warning(disable: 41012)
struct VertexInput {
    [[vk::location(0)]] float3 a_Position : POSITION;
    [[vk::location(1)]] float3 a_Normal : NORMAL;
    [[vk::location(2)]] float2 a_TexCoord : TEXCOORD;
    [[vk::location(3)]] int4 a_Joints : JOINTS;
    [[vk::location(4)]] float4 a_Weights : WEIGHTS;
    // xyz = tangent, w = handedness sign. Added for normal mapping; the loader
    // generates this per-corner and splits seam vertices so w is meaningful.
    [[vk::location(5)]] float4 a_Tangent : TANGENT;
};

struct VertexOutput {
    float4 location : SV_Position;
    [[vk::location(0)]] float3 v_Normal;
    [[vk::location(1)]] float2 v_TexCoord;
    [[vk::location(2)]] float3 v_WorldPos;
    [[vk::location(3)]] float3 v_Tangent;
    [[vk::location(4)]] float3 v_Bitangent;
};

struct PushConstants {
    float4x4 viewProjection;
    float4x4 model;
    float4 cameraPos;
    int entityID;
    int textureSlot;
    int enableLighting;
    int lightBufferIndex;
    int numLights;
    int mrTextureSlot;
    float metallicFactor;
    float roughnessFactor;
    int boneBufferIndex;
    int boneOffset;

    // Material. Total size is 232 bytes against a device limit of 256
    // (maxPushConstantsSize on GFX9), so this is close to the ceiling: a fourth
    // texture map needs a per-material descriptor, not another slot here.
    int normalTextureSlot;       // -1 when absent
    int occlusionTextureSlot;    // -1 when absent
    int emissiveTextureSlot;     // -1 when absent
    float normalScale;
    float occlusionStrength;
    float alphaCutoff;
    int alphaMode;               // 0 opaque, 1 mask, 2 blend
    float4 baseColorFactor;
    // float4, not float3, to match the C++ PushConstants layout exactly: Slang
    // and glm disagree on 3-component vector packing. .rgb is used in the shader.
    float4 emissiveFactor;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

struct LightData {
    float4 type_radius_pad; // x = type, y = radius
    float4 position_or_direction;
    float4 color_intensity;
};

[[vk::binding(0, 0)]] StructuredBuffer<LightData> u_LightBuffers[10000];
[[vk::binding(1, 0)]] Sampler2D u_Textures[10000];
[[vk::binding(0, 0)]] StructuredBuffer<float4x4> u_BoneBuffers[10000];

struct FragmentOutput {
    [[vk::location(0)]] float4 Color : SV_Target0;
    [[vk::location(1)]] int EntityID : SV_Target1;
};


[shader("vertex")]
VertexOutput vertexMain(VertexInput input) {
    VertexOutput output;
    
    float4 localPos = float4(input.a_Position, 1.0);
    float3 localNormal = input.a_Normal;
    
    if (pc.boneBufferIndex >= 0 && pc.boneOffset >= 0)
    {
        StructuredBuffer<float4x4> boneBuffer = u_BoneBuffers[pc.boneBufferIndex];
        
        float4x4 skinMat = 
            boneBuffer[pc.boneOffset + input.a_Joints.x] * input.a_Weights.x +
            boneBuffer[pc.boneOffset + input.a_Joints.y] * input.a_Weights.y +
            boneBuffer[pc.boneOffset + input.a_Joints.z] * input.a_Weights.z +
            boneBuffer[pc.boneOffset + input.a_Joints.w] * input.a_Weights.w;
            
        localPos = mul(skinMat, float4(input.a_Position, 1.0));
        localNormal = mul((float3x3)skinMat, input.a_Normal);
    }
    
    float4 worldPos = mul(pc.model, localPos);
    output.v_WorldPos = worldPos.xyz;
    output.v_Normal = normalize(mul((float3x3)pc.model, localNormal));
    output.v_TexCoord = input.a_TexCoord;

    // Tangent frame in world space. The bitangent's handedness comes from the
    // loader-generated w component, which flips on UV-mirrored seams; without it
    // normal maps light the wrong way on half the surface.
    float3 worldTangent = mul((float3x3)pc.model, input.a_Tangent.xyz);
    output.v_Tangent = normalize(worldTangent);
    output.v_Bitangent = normalize(cross(output.v_Normal, output.v_Tangent) * input.a_Tangent.w);

    output.location = mul(pc.viewProjection, worldPos);
    return output;
}

[shader("fragment")]
FragmentOutput fragmentMain(VertexOutput input) {
    FragmentOutput output;
    
    float4 texColor = u_Textures[pc.textureSlot].Sample(input.v_TexCoord);

    // baseColorFactor multiplies the base colour texture; both default to 1.
    float4 baseColor = texColor * pc.baseColorFactor;

    // Alpha MASK: discard below the cutoff. BLEND has no sorted pass yet, so it
    // falls through to normal blending and the loader has already warned.
    if (pc.alphaMode == 1) {
        if (baseColor.a < pc.alphaCutoff) {
            discard;
        }
    }

    float3 finalColor = baseColor.rgb;

    if (pc.enableLighting == 1) {
        float3 litColor = float3(0.0);
        float3 normal = normalize(input.v_Normal);

        // Normal mapping in tangent space. glTF stores normals in [-1,1] and
        // samples them from a texture, hence the *2-1 on read.
        if (pc.normalTextureSlot >= 0) {
            float3 tangentNormal = u_Textures[pc.normalTextureSlot].Sample(input.v_TexCoord).rgb * 2.0 - 1.0;
            tangentNormal.xy *= pc.normalScale;
            normal = normalize(
                tangentNormal.x * input.v_Tangent +
                tangentNormal.y * input.v_Bitangent +
                tangentNormal.z * normal
            );
        }
        float3 V = normalize(pc.cameraPos.xyz - input.v_WorldPos);
        
        // Use material properties and texture
        float metallic = pc.metallicFactor;
        float roughness = pc.roughnessFactor;
        
        if (pc.mrTextureSlot >= 0) {
            float4 mrTex = u_Textures[pc.mrTextureSlot].Sample(input.v_TexCoord);
            // In glTF, B channel is metallic, G channel is roughness
            metallic *= mrTex.b;
            roughness *= mrTex.g;
        }
        
        // Clamp roughness to prevent divide by zero
        roughness = clamp(roughness, 0.04, 0.99);
        // Clamp metallic for better default look
        metallic = clamp(metallic, 0.0, 0.99);
        
        float3 F0 = float3(0.04);
        F0 = lerp(F0, baseColor.rgb, metallic);
        
        float PI = 3.14159265359;
        float3 Lo = float3(0.0);
        
        for (int i = 0; i < pc.numLights; ++i) {
            LightData light = u_LightBuffers[pc.lightBufferIndex][i];
            
            int type = int(light.type_radius_pad.x);
            float radius = max(light.type_radius_pad.y, 0.1); // prevent div by zero

            float3 L;
            float attenuation = 1.0;
            
            if (type == 0) { // Directional Light
                L = normalize(-light.position_or_direction.xyz);
            } 
            else if (type == 1) { // Point Light
                L = light.position_or_direction.xyz - input.v_WorldPos;
                float distance = length(L);
                L = normalize(L);
                
                // Better point light attenuation (linear/quadratic mix)
                attenuation = clamp(1.0 - (distance * distance) / (radius * radius), 0.0, 1.0);
                attenuation *= attenuation; 
            }
            
            float3 H = normalize(V + L);
            // Boost radiance by PI to compensate for BRDF 1/PI division so default intensity=1 looks good
            float3 radiance = light.color_intensity.xyz * light.color_intensity.w * attenuation * PI;
            
            // Cook-Torrance BRDF
            // NDF (Distribution GGX)
            float a = roughness * roughness;
            float a2 = a * a;
            float NdotH = max(dot(normal, H), 0.0);
            float NdotH2 = NdotH * NdotH;
            
            float num = a2;
            float denom = (NdotH2 * (a2 - 1.0) + 1.0);
            denom = PI * denom * denom;
            float NDF = num / max(denom, 0.0000001);
            
            // Geometry (Smith)
            float r = (roughness + 1.0);
            float k = (r * r) / 8.0;
            
            float NdotV = max(dot(normal, V), 0.0);
            float NdotL = max(dot(normal, L), 0.0);
            
            float ggx1 = NdotV / (NdotV * (1.0 - k) + k);
            float ggx2 = NdotL / (NdotL * (1.0 - k) + k);
            float G = ggx1 * ggx2;
            
            // Fresnel (Schlick)
            float cosTheta = max(dot(H, V), 0.0);
            float3 F = F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
            
            float3 numerator = NDF * G * F;
            float denominator = 4.0 * NdotV * NdotL + 0.0001;
            float3 specular = numerator / denominator;
            
            float3 kS = F;
            float3 kD = float3(1.0) - kS;
            kD *= 1.0 - metallic;
            
            // Add lighting
            Lo += (kD * baseColor.rgb / PI + specular) * radiance * NdotL;
        }
        
        // Occlusion is a multiplier on the ambient/diffuse term only; glTF
        // defines it as cavity shading, not as a shadow.
        if (pc.occlusionTextureSlot >= 0) {
            float ao = u_Textures[pc.occlusionTextureSlot].Sample(input.v_TexCoord).r;
            // lerp, not mix: Slang follows HLSL, where mix is not defined.
            // Occlusion defaults to a strength of 1.0, which lerps to the full
            // sampled value rather than washing the ambient term out.
            ao = lerp(1.0, ao, pc.occlusionStrength);
            Lo *= ao;
        }

        // Boost ambient a bit so dark sides are visible
        float3 ambient = float3(0.08) * baseColor.rgb;
        litColor = ambient + Lo;

        // Emissive is additive and unlit, so it is added after tonemapping of the
        // lit term. emissiveFactor defaults to black, so this is a no-op unless
        // the file asks for it.
        float3 emissive = pc.emissiveFactor.rgb;
        if (pc.emissiveTextureSlot >= 0) {
            emissive *= u_Textures[pc.emissiveTextureSlot].Sample(input.v_TexCoord).rgb;
        }
        litColor += emissive;
        
        // Simple HDR tonemapping (Exposure-based instead of Reinhard for better contrast)
        float exposure = 1.0;
        litColor = float3(1.0) - exp(-litColor * exposure);
        
        finalColor = litColor;
    }
    
    output.Color = float4(finalColor, baseColor.a);
    output.EntityID = pc.entityID;
    
    return output;
}
