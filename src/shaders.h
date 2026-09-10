#pragma once

inline constexpr const char* kShaderSource = R"(
#define TILE_SIZE 8
#define EFFECT_NONE 0
#define EFFECT_IRIDESCENT 1
#define EFFECT_FROST 2
#define EFFECT_LIGHTNING 3

struct VertexIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VertexOut
{
    float4 position : SV_Position;
    float3 normalV : NORMAL;
    float2 clothUv : TEXCOORD0;
    float2 sailUv : TEXCOORD1;
    nointerpolation uint sailInstance : TEXCOORD2;
};

cbuffer FrameCB : register(b0)
{
    row_major float4x4 view;
    row_major float4x4 projection;
    row_major float4x4 viewProjection;
    row_major float4x4 inverseViewProjection;
    float4 cameraTime;
    float4 viewportDelta;
    float4 wind;
    float4 deformation;
    float4 dynamics;
    float4 effectDirectionStrength;
    uint4 sailEffectIds;
    uint debugMode;
    uint tileCountX;
    uint tileCountY;
    uint padding;
};

Texture2D clothTexture : register(t0);
Texture2D baseAlbedoTexture : register(t1);
Texture2D normalMaterialTexture : register(t2);
Texture2D<uint4> effectsVBufferTexture : register(t3);
Texture2D depthTexture : register(t4);
Texture2D signedDeltaTexture : register(t5);
SamplerState wrapSampler : register(s0);
SamplerState clampSampler : register(s1);

float3 SailInstanceOffset(uint instanceId)
{
    return float3(instanceId == 0u ? -4.8 : 0.3, 0.0, 0.0);
}

VertexOut StaticSceneVS(VertexIn input, uint instanceId : SV_InstanceID)
{
    VertexOut output;
    output.position = mul(float4(input.position + SailInstanceOffset(instanceId), 1.0), viewProjection);
    output.normalV = normalize(mul(float4(input.normal, 0.0), view).xyz);
    output.clothUv = input.uv;
    output.sailUv = 0.0;
    output.sailInstance = instanceId;
    return output;
}

VertexOut SailVS(VertexIn input, uint instanceId : SV_InstanceID)
{
    const float u = saturate(input.position.x / 4.5);
    const float v = saturate((2.25 - input.position.y) / 4.5);
    const float speed = saturate(wind.z);
    const float tension = saturate(speed * deformation.w);
    const float instanceTime = cameraTime.w + instanceId * 0.37;
    const float gust = 1.0 + wind.w * sin(instanceTime * dynamics.z * 6.2831853);
    const float waveNumber = 6.2831853 / max(0.25, deformation.y);
    const float phase = u * waveNumber + v * 2.1
                      - instanceTime * deformation.z * (0.5 + speed);
    const float wave = sin(phase) + 0.35 * sin(phase * 2.17 + 1.2);
    const float waveDerivative = cos(phase) + 0.7595 * cos(phase * 2.17 + 1.2);
    const float attached = sin(u * 1.5707963);
    const float attachedDerivative = 1.5707963 * cos(u * 1.5707963);
    const float amplitude = deformation.x * (0.25 + speed) * gust;
    const float billow = attached * amplitude * wave;
    const float billowDu = amplitude * (attachedDerivative * wave + attached * waveDerivative * waveNumber);
    const float billowDv = amplitude * attached * waveDerivative * 2.1;
    const float relaxed = (1.0 - tension) * dynamics.x;
    const float sag = dynamics.y * relaxed * u * u * (0.35 + v);
    const float sagDu = dynamics.y * relaxed * 2.0 * u * (0.35 + v);
    const float sagDv = dynamics.y * relaxed * u * u;
    const float depthScale = 0.6 + 0.4 * abs(wind.y);

    float3 position = float3(
        u * 4.5 + billow * 0.12 * wind.x,
        2.25 - v * 4.5 - sag,
        billow * depthScale);
    const float3 tangentU = float3(
        4.5 + billowDu * 0.12 * wind.x,
        -sagDu,
        billowDu * depthScale);
    const float3 tangentV = float3(
        billowDv * 0.12 * wind.x,
        -4.5 - sagDv,
        billowDv * depthScale);

    VertexOut output;
    output.position = mul(float4(position + SailInstanceOffset(instanceId), 1.0), viewProjection);
    output.normalV = normalize(mul(float4(normalize(cross(tangentV, tangentU)), 0.0), view).xyz);
    output.clothUv = input.uv;
    output.sailUv = float2(u, v);
    output.sailInstance = instanceId;
    return output;
}

struct GBufferOut
{
    float4 baseAlbedo : SV_Target0;
    float4 normalMaterial : SV_Target1;
    uint4 effects : SV_Target2;
};

uint PackHalf2(float2 value)
{
    return (f32tof16(value.x) & 0xffffu) | ((f32tof16(value.y) & 0xffffu) << 16);
}

uint PackUnorm2(float2 value)
{
    uint2 packed = (uint2)round(saturate(value) * 65535.0);
    return packed.x | (packed.y << 16);
}

float3 ReconstructSailBarycentrics(float2 uv, uint primitiveId)
{
    const float2 grid = float2(48.0, 36.0);
    float2 local = frac(min(uv, float2(0.999999, 0.999999)) * grid);
    if ((primitiveId & 1u) == 0u)
        return float3(1.0 - local.x - local.y, local.y, local.x);
    return float3(1.0 - local.y, 1.0 - local.x, local.x + local.y - 1.0);
}

GBufferOut SailGBufferPS(VertexOut input, uint primitiveId : SV_PrimitiveID)
{
    GBufferOut output;
    float3 cloth = clothTexture.Sample(wrapSampler, input.clothUv).rgb;
    float2 facePosition = float2(input.sailUv.x - 0.5, 0.5 - input.sailUv.y);
    float faceDistance = length(facePosition) - 0.285;
    float faceAa = max(fwidth(faceDistance), 0.0005);
    float faceMask = 1.0 - smoothstep(-faceAa, faceAa, faceDistance);

    float leftEyeDistance = length(facePosition - float2(-0.095, 0.075)) - 0.035;
    float rightEyeDistance = length(facePosition - float2(0.095, 0.075)) - 0.035;
    float eyeAa = max(fwidth(min(leftEyeDistance, rightEyeDistance)), 0.0005);
    float eyeMask = 1.0 - smoothstep(-eyeAa, eyeAa, min(leftEyeDistance, rightEyeDistance));

    float smileCurve = -0.105 + 1.85 * facePosition.x * facePosition.x;
    float smileDistance = abs(facePosition.y - smileCurve) - 0.014;
    float smileAa = max(fwidth(smileDistance), 0.0005);
    float smileStroke = 1.0 - smoothstep(-smileAa, smileAa, smileDistance);
    float smileWidth = 1.0 - smoothstep(0.135, 0.155, abs(facePosition.x));
    float smileMask = smileStroke * smileWidth;

    float3 faceColor = float3(0.98, 0.68, 0.08);
    float3 featureColor = float3(0.055, 0.075, 0.12);
    float3 decoratedAlbedo = lerp(cloth, lerp(cloth, faceColor, 0.88), faceMask);
    decoratedAlbedo = lerp(decoratedAlbedo, featureColor, saturate((eyeMask + smileMask) * faceMask) * 0.96);
    output.baseAlbedo = float4(decoratedAlbedo, 1.0);
    output.normalMaterial = float4(normalize(input.normalV) * 0.5 + 0.5, 1.0);
    float3 bary = max(0.0, ReconstructSailBarycentrics(input.sailUv, primitiveId));
    bary /= max(dot(bary, 1.0), 1e-5);
    output.effects = uint4(
        sailEffectIds[min(input.sailInstance, 1u)],
        primitiveId + 1u,
        PackHalf2(input.sailUv),
        PackUnorm2(bary.xy));
    return output;
}

GBufferOut MastGBufferPS(VertexOut input)
{
    GBufferOut output;
    output.baseAlbedo = float4(0.24, 0.11, 0.045, 1.0);
    output.normalMaterial = float4(normalize(input.normalV) * 0.5 + 0.5, 2.0);
    output.effects = 0u;
    return output;
}

struct FullscreenOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

FullscreenOut FullscreenVS(uint id : SV_VertexID)
{
    FullscreenOut output;
    float2 p = float2((id << 1) & 2, id & 2);
    output.uv = p;
    output.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 CompositePS(FullscreenOut input) : SV_Target
{
    uint2 pixel = min((uint2)input.position.xy, (uint2)viewportDelta.xy - 1u);
    float4 albedo = baseAlbedoTexture.Load(int3(pixel, 0));
    float4 normalMaterial = normalMaterialTexture.Load(int3(pixel, 0));
    uint4 effectsData = effectsVBufferTexture.Load(int3(pixel, 0));
    float depth = depthTexture.Load(int3(pixel, 0)).r;
    float4 delta = signedDeltaTexture.Load(int3(pixel, 0));

    if (debugMode == 1u)
        return float4(albedo.rgb, 1.0);
    if (debugMode == 2u)
        return float4(normalMaterial.rgb, 1.0);
    if (debugMode >= 3u && debugMode <= 7u)
    {
        float2 encodedUv = float2(f16tof32(effectsData.z & 0xffffu), f16tof32(effectsData.z >> 16));
        float2 bary = float2(effectsData.w & 0xffffu, effectsData.w >> 16) / 65535.0;
        float3 barycentrics = saturate(float3(bary, 1.0 - bary.x - bary.y));
        if (effectsData.y == 0u)
            return float4(0.0, 0.0, 0.0, 1.0);

        float3 idColor = 0.0;
        if (effectsData.x == EFFECT_IRIDESCENT) idColor = float3(0.0, 0.9, 1.0);
        if (effectsData.x == EFFECT_FROST) idColor = float3(0.65, 0.85, 1.0);
        if (effectsData.x == EFFECT_LIGHTNING) idColor = float3(0.2, 0.35, 1.0);

        if (debugMode == 4u)
            return float4(idColor, 1.0);
        if (debugMode == 5u)
        {
            uint primitive = effectsData.y - 1u;
            float3 primitiveColor = frac(float3(0.1031, 0.11369, 0.13787) * (primitive + 1u));
            primitiveColor += dot(primitiveColor, primitiveColor.yzx + 19.19);
            return float4(frac((primitiveColor.xxy + primitiveColor.yzz) * primitiveColor.zyx), 1.0);
        }
        if (debugMode == 6u)
            return float4(encodedUv, 0.0, 1.0);
        if (debugMode == 7u)
            return float4(barycentrics, 1.0);

        float primitivePattern = ((effectsData.y - 1u) & 31u) / 31.0;
        return float4(idColor * (0.55 + 0.45 * bary.y) +
            0.18 * float3(frac(encodedUv.x + primitivePattern), encodedUv.y, 0.0), 1.0);
    }
    if (debugMode == 8u)
        return float4(saturate(0.5 + delta.rgb), 1.0);

    if (depth >= 1.0)
        return float4(0.035, 0.075, 0.12, 1.0);

    float3 n = normalize(normalMaterial.xyz * 2.0 - 1.0);
    float diffuse = 0.25 + 0.75 * saturate(dot(n, normalize(float3(-0.35, 0.65, -0.7))));
    return float4(max(0.0, albedo.rgb * diffuse + delta.rgb), 1.0);
}

)" R"(
Texture2D<float4> computeAlbedo : register(t0);
Texture2D<float4> computeNormalMaterial : register(t1);
Texture2D<uint4> computeEffects : register(t2);
Texture2D<float> computeDepth : register(t3);
RWTexture2D<float4> outputDelta : register(u0);
RWStructuredBuffer<uint> effectTileList : register(u1);
RWStructuredBuffer<uint> indirectArgs : register(u2);

groupshared uint tileContainsEffect;

void ClassifyEffectTile(
    uint expectedEffectId,
    uint3 dispatchThreadId,
    uint3 groupThreadId,
    uint3 groupId)
{
    if (groupThreadId.x == 0u && groupThreadId.y == 0u)
        tileContainsEffect = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (dispatchThreadId.x < (uint)viewportDelta.x && dispatchThreadId.y < (uint)viewportDelta.y)
    {
        if (computeEffects.Load(int3(dispatchThreadId.xy, 0)).x == expectedEffectId)
            InterlockedOr(tileContainsEffect, 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    if (groupThreadId.x == 0u && groupThreadId.y == 0u && tileContainsEffect != 0u)
    {
        uint listIndex;
        InterlockedAdd(indirectArgs[0], 1u, listIndex);
        effectTileList[listIndex] = groupId.x | (groupId.y << 16);
    }
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void ClassifyIridescentTiles(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    ClassifyEffectTile(EFFECT_IRIDESCENT, dispatchThreadId, groupThreadId, groupId);
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void ClassifyFrostTiles(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    ClassifyEffectTile(EFFECT_FROST, dispatchThreadId, groupThreadId, groupId);
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void ClassifyLightningTiles(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    ClassifyEffectTile(EFFECT_LIGHTNING, dispatchThreadId, groupThreadId, groupId);
}

[numthreads(1, 1, 1)]
void FinalizeIndirectArgs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    indirectArgs[1] = 1u;
    indirectArgs[2] = 1u;
}

float2 DecodeSailUv(uint packed)
{
    return float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16));
}

float3 DecodeBarycentrics(uint packed)
{
    float2 firstTwo = float2(packed & 0xffffu, packed >> 16) / 65535.0;
    return float3(firstTwo, saturate(1.0 - firstTwo.x - firstTwo.y));
}

float Hash11(float value)
{
    return frac(sin(value * 127.1) * 43758.5453);
}

float Hash21(float2 value)
{
    return frac(sin(dot(value, float2(127.1, 311.7))) * 43758.5453);
}

bool ResolveEffectPixel(
    uint3 groupThreadId,
    uint3 groupId,
    uint expectedEffectId,
    out uint2 pixel,
    out uint4 effectsData)
{
    uint packedTile = effectTileList[groupId.x];
    pixel = uint2(packedTile & 0xffffu, packedTile >> 16) * TILE_SIZE + groupThreadId.xy;
    if (pixel.x >= (uint)viewportDelta.x || pixel.y >= (uint)viewportDelta.y)
    {
        effectsData = 0u;
        return false;
    }
    effectsData = computeEffects.Load(int3(pixel, 0));
    return effectsData.x == expectedEffectId;
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void IridescentWindSheenCS(
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    uint2 pixel;
    uint4 effectsData;
    if (!ResolveEffectPixel(groupThreadId, groupId, EFFECT_IRIDESCENT, pixel, effectsData))
        return;

    float2 uv = DecodeSailUv(effectsData.z);
    float3 albedo = computeAlbedo.Load(int3(pixel, 0)).rgb;
    float3 normalV = normalize(computeNormalMaterial.Load(int3(pixel, 0)).xyz * 2.0 - 1.0);
    float3 directionV = effectDirectionStrength.xyz /
        max(length(effectDirectionStrength.xyz), 1e-5);
    float facing = saturate(0.5 + 0.5 * dot(normalV, directionV));
    float phase = uv.x * 18.0 + uv.y * 7.0 + cameraTime.w * (1.4 + wind.z * 3.0) +
        facing * 8.0 + sin(uv.y * 23.0 - cameraTime.w);
    float cycle = frac(phase / 6.2831853);
    float3 cyan = float3(0.02, 0.85, 1.0);
    float3 magenta = float3(1.0, 0.04, 0.72);
    float3 gold = float3(1.0, 0.62, 0.05);
    float3 palette = cycle < 0.5
        ? lerp(cyan, magenta, cycle * 2.0)
        : lerp(magenta, gold, (cycle - 0.5) * 2.0);
    float signedWave = sin(phase * 1.7);
    float positive = max(signedWave, 0.0);
    float negative = max(-signedWave, 0.0);
    float scale = effectDirectionStrength.w / 3.0;
    float3 delta = (palette * (0.12 + 0.34 * positive) -
        albedo * (0.08 + 0.20 * negative)) * facing * scale;
    outputDelta[pixel] = float4(delta, facing);
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void FrostCrystalCS(
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    uint2 pixel;
    uint4 effectsData;
    if (!ResolveEffectPixel(groupThreadId, groupId, EFFECT_FROST, pixel, effectsData))
        return;

    float2 uv = DecodeSailUv(effectsData.z);
    float3 bary = DecodeBarycentrics(effectsData.w);
    float3 albedo = computeAlbedo.Load(int3(pixel, 0)).rgb;
    float3 normalV = normalize(computeNormalMaterial.Load(int3(pixel, 0)).xyz * 2.0 - 1.0);
    float primitiveSeed = Hash11((float)effectsData.y);
    float2 crystalCell = floor(uv * float2(42.0, 36.0));
    float crystalNoise = Hash21(crystalCell + primitiveSeed);
    float fineNoise = Hash21(floor(uv * 95.0) + cameraTime.w * 0.07);
    float advancingFront = frac(cameraTime.w * 0.055);
    float frontCoordinate = uv.y + 0.10 * sin(uv.x * 15.0 + primitiveSeed * 6.2831853);
    float front = 1.0 - smoothstep(advancingFront - 0.10, advancingFront + 0.10, frontCoordinate);
    float triangleEdge = 1.0 - smoothstep(0.015, 0.10, min(bary.x, min(bary.y, bary.z)));
    float billowEdge = pow(saturate(1.0 - abs(normalV.z)), 1.5);
    float crystals = saturate(front * (0.42 + 0.75 * crystalNoise) +
        triangleEdge * 0.65 + billowEdge * 0.35);
    float gaps = crystals * (1.0 - smoothstep(0.30, 0.58, fineNoise));
    float scale = effectDirectionStrength.w / 3.0;
    float3 iceLight = float3(0.34, 0.62, 0.88) * crystals;
    float3 darkCracks = albedo * (0.18 * gaps + 0.10 * triangleEdge);
    outputDelta[pixel] = float4((iceLight - darkCracks) * scale, crystals);
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void StormLightningCS(
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    uint2 pixel;
    uint4 effectsData;
    if (!ResolveEffectPixel(groupThreadId, groupId, EFFECT_LIGHTNING, pixel, effectsData))
        return;

    float2 uv = DecodeSailUv(effectsData.z);
    float3 bary = DecodeBarycentrics(effectsData.w);
    float3 albedo = computeAlbedo.Load(int3(pixel, 0)).rgb;
    float primitiveSeed = Hash11((float)effectsData.y * 0.731);
    float timeStep = floor(cameraTime.w * 5.0);
    float strikeSeed = Hash11(timeStep + 17.0);
    float trunk = 0.5 + 0.16 * sin(uv.y * 17.0 + strikeSeed * 6.2831853) +
        0.055 * sin(uv.y * 53.0 - cameraTime.w * 9.0 + primitiveSeed * 3.0);
    float branchDirection = primitiveSeed > 0.5 ? 1.0 : -1.0;
    float branch = trunk + branchDirection * max(0.0, uv.y - 0.34) *
        (0.28 + 0.08 * sin(uv.y * 41.0 + primitiveSeed * 8.0));
    float distanceToBolt = min(abs(uv.x - trunk), abs(uv.x - branch));
    float triangleVein = 1.0 - smoothstep(0.006, 0.028, min(bary.x, min(bary.y, bary.z)));
    float core = 1.0 - smoothstep(0.004, 0.014, distanceToBolt);
    core = saturate(core + triangleVein * 0.32 * step(0.58, primitiveSeed));
    float halo = 1.0 - smoothstep(0.014, 0.065, distanceToBolt);
    float pulse = 0.35 + 0.65 * pow(saturate(sin(cameraTime.w * 13.0 +
        strikeSeed * 12.0)), 6.0);
    float scale = effectDirectionStrength.w / 3.0;
    float3 brightCore = float3(0.58, 0.82, 1.0) * core * (0.75 + 0.65 * pulse);
    float3 darkHalo = albedo * max(halo - core, 0.0) * 0.48;
    outputDelta[pixel] = float4((brightCore - darkHalo) * scale, max(core, halo));
}
)";
