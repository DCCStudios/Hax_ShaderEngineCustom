#ifndef SHADERENGINE_SHADOW_UPGRADE_COMMON
#define SHADERENGINE_SHADOW_UPGRADE_COMMON

// Eight low-discrepancy shadow rays estimate the visibility integral of a
// finite emitter in the shadow-map projection. Screen/depth scrambling trades
// the fixed 4x4 PCF pattern for decorrelated noise without adding a resource
// outside BGS's deferred-light contract.
float SE_StochasticShadowVisibility(
    Texture2DArray<float4> shadowTexture,
    SamplerComparisonState shadowSampler,
    float3 shadowCoord,
    float receiverDepth,
    float filterRadius,
    float2 pixelPosition)
{
    const uint sampleCount = 8;
    const float goldenAngle = 2.39996323;
    const float twoPi = 6.28318531;

    float2 scramblePixel =
        pixelPosition + receiverDepth * float2(8192.0, 131071.0);
    float rotation = twoPi * frac(
        52.9829189 * frac(dot(scramblePixel, float2(0.06711056, 0.00583715))));

    float visibility = 0.0;
    [unroll]
    for (uint ray = 0; ray < sampleCount; ++ray) {
        float radius = sqrt((ray + 0.5) / sampleCount);
        float sineAngle;
        float cosineAngle;
        sincos(rotation + goldenAngle * ray, sineAngle, cosineAngle);
        float2 offset = float2(cosineAngle, sineAngle) * radius * abs(filterRadius);
        visibility += shadowTexture.SampleCmpLevelZero(
            shadowSampler,
            float3(shadowCoord.xy + offset, shadowCoord.z),
            receiverDepth);
    }
    return visibility / sampleCount;
}

// Finite-core inverse-square attenuation with a smooth radius cutoff.  The
// curve is expressed in distance/radius space, so it does not invent a game-
// unit scale, and min(vanilla, physical) guarantees the upgrade never makes a
// local light stronger than the BGS-authored result.
float SE_PhysicallyBasedLocalAttenuation(
    float distanceToLight,
    float lightRadius,
    float vanillaAttenuation)
{
    if (lightRadius <= 1.0e-4) {
        return vanillaAttenuation;
    }

    float normalizedDistance = saturate(distanceToLight / lightRadius);
    float distanceSquared = normalizedDistance * normalizedDistance;
    float radiusWindow = saturate(1.0 - distanceSquared * distanceSquared);
    radiusWindow *= radiusWindow;

    // A finite emitter core avoids the singularity at the light origin.  The
    // 0.5-radius reference is the only scale available in BGS's verified
    // position/radius contract.
    float finiteInverseSquare = 0.25 / max(distanceSquared, 0.25);
    float physicalAttenuation = saturate(finiteInverseSquare * radiusWindow);
    return min(saturate(vanillaAttenuation), physicalAttenuation);
}

#endif
