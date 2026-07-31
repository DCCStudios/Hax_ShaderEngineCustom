// Full-screen cascaded directional-shadow upgrade for Fallout 4 OG.
//
// Contract 6 reweights the isolated output from pixelDeferredLightOG.
// Contract 7 writes the upgraded visibility emitted by
// pixelShadowVisibilityOG. Both paths consume the shipped t5 shadow-map SRV
// directly for comparison sampling and raw blocker-depth reads.

Texture2D<float4> t2 : register(t2);
Texture2D<float4> t3 : register(t3);
Texture2DArray<float> t5 : register(t5);
Texture2D<float4> t26 : register(t26);
Texture2D<float4> t27 : register(t27);
Texture2D<float4> t28 : register(t28);
Texture2D<float4> t29 : register(t29);

SamplerState s2_s : register(s2);
SamplerState s3_s : register(s3);
SamplerComparisonState s5_s : register(s5);

cbuffer cb2 : register(b2)
{
    float4 cb2[28];
}

cbuffer cb12 : register(b12)
{
    float4 cb12[31];
}

cbuffer ShadowMapDeferredMode : register(b13)
{
    uint4 seShadowMapDeferredMode;
    // mode.yz = resolved projection render extent. viewport.xy = raster
    // origin, viewport.zw = reciprocal projection render extent. The depth
    // sampling UV still comes from the physical t3 allocation.
    float4 seShadowMapDeferredViewport;
}

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
#ifndef ps_TraceDirectionalFilterScale
#define ps_TraceDirectionalFilterScale 1.0
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
    // Fallout's native DRS keeps a render-sized viewport in a native-sized
    // allocation, while external upscalers can replace that allocation with a
    // physically render-sized proxy. Deriving UVs from the live t3 allocation
    // is correct for both domains; cb2[27] is not when a proxy is installed
    // after Bethesda prepared its deferred constants.
    uint width;
    uint height;
    t3.GetDimensions(width, height);
    return svPosition.xy / max(float2(width, height), 1.0.xx);
}

float2 RasterUV(float4 svPosition)
{
    return
        (svPosition.xy - seShadowMapDeferredViewport.xy) *
        seShadowMapDeferredViewport.zw;
}

float3 ReconstructBGSWorldPosition(
    float4 svPosition,
    float rawDepth,
    out float projectionDepth)
{
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;

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

    float2 rasterUV = RasterUV(svPosition);
    float2 clip;
    clip.x = rasterUV.x;
    clip.y = 1.0 - rasterUV.y;
    float4 projected = float4(clip * 2.0 - 1.0, projectionDepth, 1.0);
    float4 world;
    world.x = dot(row0, projected);
    world.y = dot(row1, projected);
    world.z = dot(row2, projected);
    world.w = dot(row3, projected);
    return world.xyz / max(abs(world.w), 1e-6) * sign(world.w);
}

ShadowProjection ProjectMainCascade(
    float3 worldPosition,
    uint cascade)
{
    float4 world = float4(worldPosition, 1.0);
    uint row = cascade == 0 ? 11u : 14u;
    float span =
        cb2[21u + cascade].w - cb2[21u + cascade].z;
    float bias = (cascade == 0 ? 0.275 : 1.0) /
        max(abs(span), 1e-5);

    ShadowProjection result;
    result.uv.x = dot(cb2[row], world);
    result.uv.y = dot(cb2[row + 1u], world);
    result.receiverDepth = dot(cb2[row + 2u], world) - bias;
    result.slice = cascade;
    result.valid =
        step(0.0, result.uv.x) * step(result.uv.x, 1.0) *
        step(0.0, result.uv.y) * step(result.uv.y, 1.0);
    return result;
}

ShadowProjection ProjectVisibilityCascade(
    float3 worldPosition,
    float materialTag)
{
    float4 world = float4(worldPosition, 1.0);
    uint cascade = min((uint)cb2[9].y, 1u);
    float span =
        cb2[21u + cascade].w - cb2[21u + cascade].z;
    float normalBias =
        abs(materialTag - 1.0) < 0.25 ? 0.08 : 0.275;

    ShadowProjection result;
    result.uv.x = dot(cb2[11], world);
    result.uv.y = dot(cb2[12], world);
    result.receiverDepth =
        min(0.999999, dot(cb2[13], world)) -
        normalBias / max(abs(span), 1e-5);
    result.slice = cascade;
    result.valid =
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

float VanillaVisibility(ShadowProjection projection)
{
    if (projection.valid < 0.5)
    {
        return 1.0;
    }

    float visibility = 0.0;
    float radius = 6.0 * cb2[20].z;
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
    return t5.Load(int4(texel, projection.slice, 0));
}

float PCSSVisibility(ShadowProjection projection)
{
    if (projection.valid < 0.5)
    {
        return 1.0;
    }

    uint width;
    uint height;
    uint layers;
    t5.GetDimensions(width, height, layers);
    if (width == 0 || height == 0 || projection.slice >= layers)
    {
        return 1.0;
    }

    uint2 dimensions = uint2(width, height);
    float2 texel = rcp(float2(dimensions));
    float2 sourceRadius =
        max(
            abs(cb2[20].zz) * 6.0 *
                max(ps_TraceDirectionalFilterScale, 0.0),
            texel);
    float blockerDepth = 0.0;
    float blockerCount = 0.0;

    [unroll(8)]
    for (int blockerIndex = 0; blockerIndex < 8; ++blockerIndex)
    {
        float depth = RawShadowDepth(
            projection,
            (kDisk[blockerIndex] - 0.5) * sourceRadius,
            dimensions);
        float isBlocker =
            step(depth + 1e-5, projection.receiverDepth);
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
    float2 filterRadius = clamp(
        sourceRadius * max(penumbra, 0.20),
        max(sourceRadius * 0.75, texel),
        max(sourceRadius * 4.0, texel));

    float visibility = 0.0;
    [unroll(16)]
    for (int filterIndex = 0; filterIndex < 16; ++filterIndex)
    {
        visibility += CompareShadow(
            projection,
            (kDisk[filterIndex] - 0.5) * filterRadius);
    }
    return visibility / 16.0;
}

float BlendCascades(
    float projectionDepth,
    float nearVisibility,
    float farVisibility)
{
    if (projectionDepth < cb2[10].x)
    {
        return nearVisibility;
    }
    if (projectionDepth > cb2[10].y)
    {
        return farVisibility;
    }

    float range = max(cb2[10].y - cb2[10].x, 1e-5);
    float blend = saturate(
        (projectionDepth - cb2[10].x) / range);
    blend = blend * blend * (3.0 - 2.0 * blend);
    return lerp(nearVisibility, farVisibility, blend);
}

float DistanceFade(float3 worldPosition)
{
    float fade = saturate(
        dot(worldPosition, worldPosition) /
        max(cb2[24].x, 1e-5));
    fade *= fade;
    fade *= fade;
    return 1.0 - fade * fade;
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

float CorrectedVisibility(
    float vanillaVisibility,
    float upgradedVisibility)
{
    float strength = max(0.0, ps_TraceDirectStrength);
    float targetVisibility = saturate(
        vanillaVisibility +
        (upgradedVisibility - vanillaVisibility) * strength);
    float maximumCorrection = saturate(ps_TraceMaxCorrection);
    return clamp(
        targetVisibility,
        vanillaVisibility - maximumCorrection,
        vanillaVisibility + maximumCorrection);
}

DeferredOutput main(float4 svPosition : SV_POSITION0)
{
    DeferredOutput output;
    int2 pixel = int2(svPosition.xy);
    float4 unshadowedDiffuse = t26.Load(int3(pixel, 0));
    float4 unshadowedSpecular = t27.Load(int3(pixel, 0));
    float4 isolatedDiffuse = t28.Load(int3(pixel, 0));
    float4 isolatedSpecular = t29.Load(int3(pixel, 0));
    float2 screenUV = ScreenUV(svPosition);
    float rawDepth = t3.SampleGrad(
        s3_s,
        screenUV,
        ddx_coarse(screenUV),
        ddy_coarse(screenUV)).x;
    float projectionDepth;
    float3 worldPosition =
        ReconstructBGSWorldPosition(
            svPosition, rawDepth, projectionDepth);
    float materialTag =
        t2.SampleLevel(s2_s, screenUV, 0).w * 255.0;

    uint contract = seShadowMapDeferredMode.x;
    float vanillaRaw;
    float upgradedRaw;
    if (contract == 7u)
    {
        ShadowProjection projection =
            ProjectVisibilityCascade(worldPosition, materialTag);
        vanillaRaw = VanillaVisibility(projection);
        upgradedRaw = PCSSVisibility(projection);
    }
    else
    {
        ShadowProjection nearProjection =
            ProjectMainCascade(worldPosition, 0u);
        ShadowProjection farProjection =
            ProjectMainCascade(worldPosition, 1u);
        vanillaRaw = BlendCascades(
            projectionDepth,
            VanillaVisibility(nearProjection),
            VanillaVisibility(farProjection));
        upgradedRaw = BlendCascades(
            projectionDepth,
            PCSSVisibility(nearProjection),
            PCSSVisibility(farProjection));
    }

    float fade = DistanceFade(worldPosition);
    vanillaRaw = 1.0 + fade * (vanillaRaw - 1.0);
    upgradedRaw = 1.0 + fade * (upgradedRaw - 1.0);
    float vanillaVisibility =
        MaterialAdjustedVisibility(vanillaRaw, materialTag, false);
    float upgradedVisibility =
        MaterialAdjustedVisibility(upgradedRaw, materialTag, true);
    float targetVisibility =
        CorrectedVisibility(vanillaVisibility, upgradedVisibility);

    if (contract == 7u)
    {
        output.diffuse = targetVisibility.xxxx;
        output.specular = float4(targetVisibility.xxx, 1.0);
        return output;
    }

    // The directional lighting permutation is not a pure direct-light buffer:
    //
    //   shadowed   = ambient + direct * vanillaVisibility
    //   unshadowed = ambient + direct
    //
    // The CPU replay supplies both observations. Solve for direct radiance and
    // change only its visibility, preserving ambient/SH rather than multiplying
    // the entire deferred result by target/vanilla. At full visibility the two
    // observations are degenerate, so retain the original instead of inventing
    // a direct/ambient split.
    if (vanillaVisibility < 1.0 - 1e-4)
    {
        float denominator = max(1.0 - vanillaVisibility, 1e-4);
        float3 diffuseDirect =
            max(unshadowedDiffuse.rgb - isolatedDiffuse.rgb, 0.0) /
            denominator;
        float3 specularDirect =
            max(unshadowedSpecular.rgb - isolatedSpecular.rgb, 0.0) /
            denominator;
        output.diffuse = isolatedDiffuse;
        output.specular = isolatedSpecular;
        output.diffuse.rgb = max(
            isolatedDiffuse.rgb +
                diffuseDirect * (targetVisibility - vanillaVisibility),
            0.0);
        output.specular.rgb = max(
            isolatedSpecular.rgb +
                specularDirect * (targetVisibility - vanillaVisibility),
            0.0);
    }
    else
    {
        output.diffuse = isolatedDiffuse;
        output.specular = isolatedSpecular;
    }
    return output;
}
