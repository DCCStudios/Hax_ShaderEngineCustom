// Compile-only harness for the runtime-assembled deferred-light processors.
// ShaderEngine normally prepends the complete generated injected-data
// declaration and modular Values.ini macros before compiling these files.

struct SEValidationInjected
{
    float g_Random;
    float3 padding0;
    float4 g_InvProjRow0;
    float4 g_InvProjRow1;
    float4 g_InvProjRow2;
    float4 g_InvProjRow3;
    float4 g_InvViewRow0;
    float4 g_InvViewRow1;
    float4 g_InvViewRow2;
    float4 g_InvViewRow3;
    float4 g_ViewProjRow0;
    float4 g_ViewProjRow1;
    float4 g_ViewProjRow2;
    float4 g_ViewProjRow3;
    float4 g_CurrentCameraPositionAdjust;
};

StructuredBuffer<SEValidationInjected> GFXInjected : register(t31);

float3 ReconstructWorldPos(float2 uv, float rawDepth)
{
    float4 clipPosition = float4(
        uv.x * 2.0 - 1.0,
        (1.0 - uv.y) * 2.0 - 1.0,
        rawDepth,
        1.0);
    float4x4 inverseProjection = float4x4(
        GFXInjected[0].g_InvProjRow0,
        GFXInjected[0].g_InvProjRow1,
        GFXInjected[0].g_InvProjRow2,
        GFXInjected[0].g_InvProjRow3);
    float4 viewPosition = mul(
        clipPosition,
        inverseProjection);
    viewPosition.xyz *=
        abs(viewPosition.w) > 1e-6 ?
            rcp(viewPosition.w) :
            1.0;
    viewPosition.w = 1.0;
    float4x4 inverseView = float4x4(
        GFXInjected[0].g_InvViewRow0,
        GFXInjected[0].g_InvViewRow1,
        GFXInjected[0].g_InvViewRow2,
        GFXInjected[0].g_InvViewRow3);
    float4 worldPosition = mul(
        viewPosition,
        inverseView);
    return worldPosition.xyz *
        (abs(worldPosition.w) > 1e-6 ?
            rcp(worldPosition.w) :
            1.0);
}

#define ps_ToonSkinEnabled false
#define ps_SkinRampFeather 0.1
#define ps_SkinRampMidpoint 0.5
#define ps_SkinShadowTone 1.0
#define ps_SkinDirectToonStrength 0.0
#define ps_CharacterShadowLift 0.0
#define ps_EquipmentShadowLift 0.0
#define ps_FaceShadowLift 0.0
#define ps_TraceDirectStrength 1.0
#define ps_TraceMaxCorrection 1.0
#define ps_TraceLocalLightCount 2
#define ps_TraceRaySteps 8
#define ps_TraceThicknessScale 1.0
#define ps_TraceDirectionalFilterScale 1.0
#define ps_TraceLocalFilterScale 1.0

#if SE_VALIDATION_SHADER == 1
#include "ShadowMapDeferredLighting.hlsl"
#elif SE_VALIDATION_SHADER == 2
#include "DirectionalShadowMapDeferredLighting.hlsl"
#elif SE_VALIDATION_SHADER == 3
#include "TiledDeferredLighting.hlsl"
#else
#error SE_VALIDATION_SHADER must be 1, 2, or 3.
#endif
