// Screen-space visibility processor for Fallout 4's native tiled-light
// accumulators. The engine's selected deferred-composite permutation remains
// authoritative; this pass only produces filtered copies of its t11/t12
// diffuse/specular inputs.

Texture2D<float> t7 : register(t7);
Texture2D<float4> t11 : register(t11);
Texture2D<float4> t12 : register(t12);

struct SETiledLight
{
    float4 positionAndRadius; // xyz = absolute BGS world position
    float4 colorAndFlags;     // rgb = light color
    float4 directionAndId;
};

StructuredBuffer<SETiledLight> seTiledLights : register(t25);

cbuffer TiledDeferredMode : register(b13)
{
    uint4 seTiledDeferredMode; // x = captured light count
}

#ifndef ps_TraceLocalLightCount
#define ps_TraceLocalLightCount 2
#endif
#ifndef ps_TraceRaySteps
#define ps_TraceRaySteps 8
#endif
#ifndef ps_TraceThicknessScale
#define ps_TraceThicknessScale 1.0
#endif
#ifndef ps_TraceDirectStrength
#define ps_TraceDirectStrength 1.0
#endif
#ifndef ps_TraceMaxCorrection
#define ps_TraceMaxCorrection 1.0
#endif

static const uint kMaximumLights = 16;
static const uint kMaximumRaySteps = 16;

struct TiledOutput
{
    float4 diffuse : SV_Target0;
    float4 specular : SV_Target1;
};

bool Finite3(float3 value)
{
    return all(value == value) && all(abs(value) < 3.402823e+37);
}

bool IsSky(float rawDepth)
{
    return rawDepth <= 0.0001 || rawDepth >= 0.99999;
}

float SampleDepth(float2 uv)
{
    uint width;
    uint height;
    t7.GetDimensions(width, height);
    int2 pixel = int2(saturate(uv) * float2(width, height));
    pixel = clamp(pixel, int2(0, 0), int2(width, height) - 1);
    return t7.Load(int3(pixel, 0));
}

float3 WorldToNDC(float3 cameraRelativePosition)
{
    float4x4 transform = float4x4(
        GFXInjected[0].g_ViewProjRow0,
        GFXInjected[0].g_ViewProjRow1,
        GFXInjected[0].g_ViewProjRow2,
        GFXInjected[0].g_ViewProjRow3);
    float4 clip = mul(float4(cameraRelativePosition, 1.0), transform);
    return clip.xyz *
        rcp(max(abs(clip.w), 1e-6)) *
        sign(clip.w);
}

float2 NDCToUV(float3 ndc)
{
    return float2(
        ndc.x * 0.5 + 0.5,
        0.5 - ndc.y * 0.5);
}

float LightWeight(
    SETiledLight light,
    float3 receiverPosition,
    out float3 lightPosition,
    out float distanceToLight)
{
    lightPosition =
        light.positionAndRadius.xyz -
        GFXInjected[0].g_CurrentCameraPositionAdjust.xyz;
    float3 delta = lightPosition - receiverPosition;
    distanceToLight = length(delta);
    float radius = light.positionAndRadius.w;
    if (!(radius > 0.0) ||
        !(distanceToLight > 1e-3) ||
        distanceToLight >= radius) {
        return 0.0;
    }

    float falloff = saturate(1.0 - distanceToLight / radius);
    float luminance = dot(
        max(0.0.xxx, light.colorAndFlags.rgb),
        float3(0.2126, 0.7152, 0.0722));
    return luminance * falloff * falloff;
}

// Returns true only when the full useful segment remains on screen and the
// depth buffer provides a geometrically plausible blocker between receiver
// and light. Off-screen and invalid rays intentionally retain native light.
bool TraceLocalVisibility(
    float3 receiverPosition,
    float3 lightPosition,
    float distanceToLight,
    uint requestedSteps,
    float jitter,
    out bool traceValid)
{
    float3 direction =
        (lightPosition - receiverPosition) /
        max(distanceToLight, 1e-4);
    float thickness = clamp(
        distanceToLight * 0.0125 *
            max(ps_TraceThicknessScale, 0.05),
        3.0,
        48.0);
    float3 origin =
        receiverPosition + direction * thickness * 2.0;
    float usefulDistance = max(
        0.0,
        distanceToLight - thickness * 3.0);
    uint stepCount = clamp(
        requestedSteps,
        4u,
        kMaximumRaySteps);
    traceValid = usefulDistance > thickness;
    if (!traceValid) {
        return false;
    }

    [loop]
    for (uint stepIndex = 0;
         stepIndex < kMaximumRaySteps;
         ++stepIndex) {
        if (stepIndex >= stepCount) {
            break;
        }

        float t =
            (float(stepIndex) + lerp(0.35, 0.85, jitter)) /
            float(stepCount);
        float distanceAlongRay = usefulDistance * t;
        float3 rayPosition =
            origin + direction * distanceAlongRay;
        float3 ndc = WorldToNDC(rayPosition);
        if (any(abs(ndc.xy) >= 0.998) ||
            ndc.z <= 0.0001 ||
            ndc.z >= 0.99999) {
            traceValid = false;
            return false;
        }

        float2 uv = NDCToUV(ndc);
        float rawDepth = SampleDepth(uv);
        if (IsSky(rawDepth)) {
            continue;
        }

        float3 scenePosition =
            ReconstructWorldPos(uv, rawDepth);
        float blockerDistance =
            dot(scenePosition - origin, direction);
        float3 closestPoint =
            origin + direction * blockerDistance;
        float lateralDistance =
            length(scenePosition - closestPoint);
        float acceptance =
            max(2.0, thickness * 0.8);

        if (blockerDistance > thickness &&
            blockerDistance < usefulDistance &&
            lateralDistance < acceptance) {
            return true;
        }
    }
    return false;
}

TiledOutput main(float4 svPosition : SV_POSITION)
{
    uint width;
    uint height;
    t7.GetDimensions(width, height);
    int2 pixel = clamp(
        int2(svPosition.xy),
        int2(0, 0),
        int2(width, height) - 1);

    TiledOutput output;
    output.diffuse = t11.Load(int3(pixel, 0));
    output.specular = t12.Load(int3(pixel, 0));

    uint lightCount = min(
        seTiledDeferredMode.x,
        min(
            (uint)clamp(
                ps_TraceLocalLightCount,
                0,
                (int)kMaximumLights),
            kMaximumLights));
    if (lightCount == 0 ||
        !any(GFXInjected[0].g_ViewProjRow3 != 0)) {
        return output;
    }

    float rawDepth = t7.Load(int3(pixel, 0));
    if (IsSky(rawDepth)) {
        return output;
    }

    float2 uv =
        (float2(pixel) + 0.5) /
        float2(width, height);
    float3 receiverPosition =
        ReconstructWorldPos(uv, rawDepth);
    if (!Finite3(receiverPosition)) {
        return output;
    }

    float totalTracedWeight = 0.0;
    float blockedWeight = 0.0;
    float jitter = frac(
        sin(dot(
            float2(pixel) + GFXInjected[0].g_Random,
            float2(12.9898, 78.233))) *
        43758.5453);

    [loop]
    for (uint lightIndex = 0;
         lightIndex < kMaximumLights;
         ++lightIndex) {
        if (lightIndex >= lightCount) {
            break;
        }

        SETiledLight light = seTiledLights[lightIndex];
        float3 lightPosition;
        float distanceToLight;
        float weight = LightWeight(
            light,
            receiverPosition,
            lightPosition,
            distanceToLight);
        if (!(weight > 1e-5)) {
            continue;
        }

        bool traceValid;
        bool blocked = TraceLocalVisibility(
            receiverPosition,
            lightPosition,
            distanceToLight,
            (uint)max(ps_TraceRaySteps, 0),
            frac(jitter + lightIndex * 0.61803398875),
            traceValid);
        if (!traceValid) {
            continue;
        }
        totalTracedWeight += weight;
        blockedWeight += blocked ? weight : 0.0;
    }

    if (!(totalTracedWeight > 1e-5)) {
        return output;
    }

    float blockedRatio =
        saturate(blockedWeight / totalTracedWeight);
    float correction =
        blockedRatio *
        max(ps_TraceDirectStrength, 0.0) *
        saturate(ps_TraceMaxCorrection);
    correction = min(correction, 0.9);
    float visibility = 1.0 - correction;

    output.diffuse.rgb =
        max(0.0.xxx, output.diffuse.rgb * visibility);
    output.specular.rgb =
        max(0.0.xxx, output.specular.rgb * visibility);
    return output;
}
