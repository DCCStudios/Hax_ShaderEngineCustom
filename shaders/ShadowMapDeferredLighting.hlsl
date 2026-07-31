// Per-shadowed-local-light deferred composite for Fallout 4 OG.
//
// The pass runs for local lights in both interiors and exteriors. It first
// isolates one vanilla/HachiToon light into t28/t29, then repeats the same
// light-volume raster with this shader. t4/t5 and b2/b12 are the still-live
// BGS shadow-map, projection, light and camera contracts.

Texture2D<float4> t2 : register(t2);
Texture2D<float4> t3 : register(t3);
Texture2DArray<float> t4 : register(t4);
Texture2DArray<float> t5 : register(t5);
Texture2D<float4> t28 : register(t28);
Texture2D<float4> t29 : register(t29);

SamplerState s2_s : register(s2);
SamplerState s3_s : register(s3);
SamplerComparisonState s5_s : register(s5);

cbuffer cb2 : register(b2)
{
    float4 cb2[23];
}

cbuffer cb12 : register(b12)
{
    float4 cb12[30];
}

cbuffer ShadowMapDeferredMode : register(b13)
{
    uint4 seShadowMapDeferredMode;
}

// Keep this renderer-side pass usable when HachiToon is absent while allowing
// ShaderEngine's generated value macros to remain authoritative when present.
#ifndef ps_ToonSkinEnabled
#define ps_ToonSkinEnabled false
#endif
#ifndef ps_SkinRampFeather
#define ps_SkinRampFeather 0.1
#endif
#ifndef ps_SkinRampMidpoint
#define ps_SkinRampMidpoint 0.5
#endif
#ifndef ps_SkinShadowTone
#define ps_SkinShadowTone 1.0
#endif
#ifndef ps_SkinDirectToonStrength
#define ps_SkinDirectToonStrength 0.0
#endif
#ifndef ps_CharacterShadowLift
#define ps_CharacterShadowLift 0.0
#endif
#ifndef ps_EquipmentShadowLift
#define ps_EquipmentShadowLift 0.0
#endif
#ifndef ps_FaceShadowLift
#define ps_FaceShadowLift 0.0
#endif
#ifndef ps_TraceDirectStrength
#define ps_TraceDirectStrength 1.0
#endif
#ifndef ps_TraceMaxCorrection
#define ps_TraceMaxCorrection 1.0
#endif

#include "deferredLightSkinCommon.inc"

static const float2 kDisk[16] =
{
    float2(0.493393, 0.394269),
    float2(0.798547, 0.885922),
    float2(0.247322, 0.926450),
    float2(0.051454, 0.140782),
    float2(0.831843, 0.009552),
    float2(0.428632, 0.017151),
    float2(0.015656, 0.749779),
    float2(0.758385, 0.496170),
    float2(0.223487, 0.562151),
    float2(0.011628, 0.406995),
    float2(0.241462, 0.304636),
    float2(0.430311, 0.727226),
    float2(0.981811, 0.278359),
    float2(0.407056, 0.500534),
    float2(0.123478, 0.463546),
    float2(0.809534, 0.682272)
};

struct ShadowProjection
{
    float2 uv;
    float receiverDepth;
    uint slice;
    float valid;
};

struct DeferredOutput
{
    float4 diffuse : SV_Target0;
    float4 specular : SV_Target1;
};

float2 ScreenUV(float4 svPosition)
{
    return cb2[22].xy * svPosition.xy * cb2[0].xy;
}

float3 ReconstructBGSWorldPosition(float4 svPosition, float rawDepth)
{
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
    float projectionDepth;

    if (rawDepth <= 0.01)
    {
        projectionDepth = rawDepth * 100.0;
        row0 = cb12[24];
        row1 = cb12[25];
        row2 = cb12[26];
        row3 = cb12[27];
    }
    else
    {
        projectionDepth = rawDepth * 1.01 - 0.01;
        row0 = cb12[20];
        row1 = cb12[21];
        row2 = cb12[22];
        row3 = cb12[23];
    }

    float2 clip;
    clip.x = cb2[0].x * svPosition.x;
    clip.y = 1.0 - cb2[0].y * svPosition.y;
    float4 projected = float4(clip * 2.0 - 1.0, projectionDepth, 1.0);
    float4 world;
    world.x = dot(row0, projected);
    world.y = dot(row1, projected);
    world.z = dot(row2, projected);
    world.w = dot(row3, projected);
    return world.xyz / max(abs(world.w), 1e-6) * sign(world.w);
}

ShadowProjection ProjectPlanar(float3 worldPosition)
{
    float4 world = float4(worldPosition, 1.0);
    float4 lightClip;
    lightClip.x = dot(cb2[11], world);
    lightClip.y = dot(cb2[12], world);
    lightClip.z = dot(cb2[13], world);
    lightClip.w = dot(cb2[14], world);

    ShadowProjection result;
    float inverseW = rcp(max(abs(lightClip.w), 1e-6)) * sign(lightClip.w);
    float3 projected = lightClip.xyz * inverseW;
    result.uv = projected.xy * 0.5 + 0.5;
    result.receiverDepth = projected.z - cb2[15].x;
    result.slice = 0;
    result.valid =
        step(1e-6, abs(lightClip.w)) *
        step(0.0, result.uv.x) * step(result.uv.x, 1.0) *
        step(0.0, result.uv.y) * step(result.uv.y, 1.0);
    return result;
}

ShadowProjection ProjectPoint(float3 worldPosition)
{
    float4 world = float4(worldPosition, 1.0);
    float3 lightVector;
    lightVector.x = dot(cb2[11], world);
    lightVector.y = dot(cb2[12], world);
    lightVector.z = dot(cb2[13], world);
    float lightW = dot(cb2[14], world);

    bool backHemisphere = (lightVector.z * 0.5 + 0.5) < 0.0;
    float inverseW = rcp(max(abs(lightW), 1e-6)) * sign(lightW);
    lightVector *= inverseW;
    float projectedLength = length(lightVector);
    float3 direction = lightVector / max(projectedLength, 1e-6);
    direction += backHemisphere ? float3(0.0, 0.0, -1.0)
                                : float3(0.0, 0.0, 1.0);
    direction = normalize(direction);

    float2 paraboloid = direction.xy / max(abs(direction.z), 1e-6) *
                        sign(direction.z);
    paraboloid = paraboloid * 0.5 + 0.5;
    float selectedY = backHemisphere ? paraboloid.y : 1.0 - paraboloid.y;
    float atlasScale = cb2[20].z;

    ShadowProjection result;
    result.uv = float2(
        atlasScale * paraboloid.x,
        1.0 - atlasScale * selectedY);
    result.receiverDepth =
        saturate(projectedLength / max(cb2[1].w, 1e-6)) - cb2[15].x;
    result.slice = backHemisphere ? 1u : 0u;
    result.valid =
        step(1e-6, abs(lightW)) *
        step(0.0, result.uv.x) * step(result.uv.x, 1.0) *
        step(0.0, result.uv.y) * step(result.uv.y, 1.0);
    return result;
}

float CompareShadow(
    ShadowProjection projection,
    float2 offset)
{
    return t5.SampleCmpLevelZero(
        s5_s,
        float3(projection.uv + offset, projection.slice),
        projection.receiverDepth);
}

float VanillaVisibility(
    ShadowProjection projection,
    uint filterMode)
{
    if (filterMode == 0)
    {
        return CompareShadow(projection, float2(0.0, 0.0));
    }

    if (filterMode == 1)
    {
        float visibility = 0.0;
        [unroll(3)]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll(3)]
            for (int x = -1; x <= 1; ++x)
            {
                visibility += CompareShadow(
                    projection,
                    float2(x, y) * cb2[15].zw);
            }
        }
        return visibility / 9.0;
    }

    float visibility = 0.0;
    float radius = 6.0 * cb2[15].z;
    [unroll(16)]
    for (int i = 0; i < 16; ++i)
    {
        visibility += CompareShadow(
            projection,
            (kDisk[i] - 0.5) * radius);
    }
    return visibility / 16.0;
}

float RawShadowDepth(
    ShadowProjection projection,
    float2 offset,
    uint2 dimensions)
{
    float2 uv = saturate(projection.uv + offset);
    int2 texel = min(
        int2(uv * dimensions),
        int2(dimensions) - 1);
    return t4.Load(int4(texel, projection.slice, 0));
}

float2 BGSFilterRadius(uint filterMode, uint2 dimensions)
{
    float2 texel = rcp(float2(dimensions));
    if (filterMode == 0)
    {
        return texel * 2.0;
    }
    if (filterMode == 1)
    {
        return max(abs(cb2[15].zw) * 2.0, texel * 2.0);
    }
    return max(abs(cb2[15].zz) * 6.0, texel * 2.0);
}

float PCSSVisibility(
    ShadowProjection projection,
    uint filterMode)
{
    uint width;
    uint height;
    uint layers;
    t4.GetDimensions(width, height, layers);
    if (width == 0 || height == 0 || projection.slice >= layers)
    {
        return 1.0;
    }

    uint2 dimensions = uint2(width, height);
    float2 sourceRadius = BGSFilterRadius(filterMode, dimensions);
    float blockerDepth = 0.0;
    float blockerCount = 0.0;

    [unroll(8)]
    for (int i = 0; i < 8; ++i)
    {
        float depth = RawShadowDepth(
            projection,
            (kDisk[i] - 0.5) * sourceRadius,
            dimensions);
        float isBlocker = step(depth + 1e-5, projection.receiverDepth);
        blockerDepth += depth * isBlocker;
        blockerCount += isBlocker;
    }

    if (blockerCount < 0.5)
    {
        return 1.0;
    }

    blockerDepth /= blockerCount;
    float penumbra =
        max(projection.receiverDepth - blockerDepth, 0.0) /
        max(abs(blockerDepth), 1e-4);
    float2 texel = rcp(float2(dimensions));
    float2 filterRadius = clamp(
        sourceRadius * max(penumbra, 0.25),
        texel * 0.5,
        max(sourceRadius * 4.0, texel));

    float visibility = 0.0;
    [unroll(16)]
    for (int i = 0; i < 16; ++i)
    {
        visibility += CompareShadow(
            projection,
            (kDisk[i] - 0.5) * filterRadius);
    }
    return visibility / 16.0;
}

float MaterialAdjustedVisibility(
    float visibility,
    float materialTag,
    bool includeFaceLift)
{
    float equipmentWeight = saturate(1.0 - abs(materialTag - 9.0));
    float characterWeight =
        saturate(1.0 - abs(materialTag - 5.0)) +
        saturate(1.0 - abs(materialTag - 6.0)) +
        saturate(1.0 - abs(materialTag - 7.0));
    characterWeight = saturate(characterWeight);

    visibility =
        ApplyEquipmentShadowVisibility(visibility, equipmentWeight);
    visibility =
        ApplyCharacterShadowLift(visibility, characterWeight);
    if (includeFaceLift && ps_ToonSkinEnabled)
    {
        float faceWeight = saturate(1.0 - abs(materialTag - 6.0));
        visibility = lerp(
            visibility,
            1.0,
            saturate(ps_FaceShadowLift) * faceWeight);
    }
    return saturate(visibility);
}

DeferredOutput main(float4 svPosition : SV_POSITION0)
{
    DeferredOutput output;
    int2 pixel = int2(svPosition.xy);
    float4 isolatedDiffuse = t28.Load(int3(pixel, 0));
    float4 isolatedSpecular = t29.Load(int3(pixel, 0));

    if (all(isolatedDiffuse == 0.0) && all(isolatedSpecular == 0.0))
    {
        output.diffuse = 0.0;
        output.specular = 0.0;
        return output;
    }

    float2 screenUV = ScreenUV(svPosition);
    float rawDepth = t3.SampleGrad(
        s3_s,
        screenUV,
        ddx_coarse(screenUV),
        ddy_coarse(screenUV)).x;
    float3 worldPosition =
        ReconstructBGSWorldPosition(svPosition, rawDepth);

    uint contract = seShadowMapDeferredMode.x;
    bool pointProjection = contract >= 3;
    uint filterMode = pointProjection ? contract - 3 : contract;
    ShadowProjection projection =
        pointProjection ? ProjectPoint(worldPosition)
                        : ProjectPlanar(worldPosition);
    if (projection.valid < 0.5)
    {
        output.diffuse = isolatedDiffuse;
        output.specular = isolatedSpecular;
        return output;
    }

    float vanillaRaw = VanillaVisibility(projection, filterMode);
    float upgradedRaw = PCSSVisibility(projection, filterMode);
    float materialTag = t2.SampleLevel(s2_s, screenUV, 0).w * 255.0;

    // The isolated source already contains the maintained HachiToon equipment
    // and character lifts. Reproduce that exact denominator, then add the
    // face-specific lift only to the upgraded numerator.
    float vanillaVisibility =
        MaterialAdjustedVisibility(vanillaRaw, materialTag, false);
    float upgradedVisibility =
        MaterialAdjustedVisibility(upgradedRaw, materialTag, true);

    float strength = max(0.0, ps_TraceDirectStrength);
    float targetVisibility = saturate(
        vanillaVisibility +
        (upgradedVisibility - vanillaVisibility) * strength);
    float maximumCorrection = saturate(ps_TraceMaxCorrection);
    targetVisibility = clamp(
        targetVisibility,
        vanillaVisibility - maximumCorrection,
        vanillaVisibility + maximumCorrection);

    float ratio;
    if (vanillaVisibility > 1e-4)
    {
        ratio = targetVisibility / vanillaVisibility;
    }
    else
    {
        // Scratch contains only the already shaded light. With no measurable
        // source visibility there is no validated unshadowed radiance to
        // reconstruct, so preserve the isolated result instead of inventing
        // energy through an arbitrary amplification factor.
        ratio = 1.0;
    }

    output.diffuse = isolatedDiffuse * ratio;
    output.specular = isolatedSpecular * ratio;
    return output;
}
