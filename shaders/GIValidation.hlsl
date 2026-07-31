// Compile-only harness for HachiToon's runtime-assembled SSRTGI passes.
// ShaderEngine normally prepends the complete injected-data declaration and
// Values.ini accessors before compiling the package shader.

struct SEGIValidationInjected
{
    float g_Frame;
    float3 g_CameraPos;
    float4 g_ViewProjRow0;
    float4 g_ViewProjRow1;
    float4 g_ViewProjRow2;
    float4 g_ViewProjRow3;
    float4 g_PrevViewProjRow0;
    float4 g_PrevViewProjRow1;
    float4 g_PrevViewProjRow2;
    float4 g_PrevViewProjRow3;
    float4 g_CurrentCameraPositionAdjust;
    float4 g_PreviousCameraPositionAdjust;
    float4 g_RenderInfo;
    float4 g_SH_R;
    float4 g_SH_G;
    float4 g_SH_B;
    float g_SunR;
    float g_SunG;
    float g_SunB;
    float g_SunDirX;
    float g_SunDirY;
    float g_SunDirZ;
    float g_SunValid;
};

StructuredBuffer<SEGIValidationInjected> GFXInjected : register(t31);

float3 ReconstructWorldPos(float2 uv, float rawDepth)
{
    return float3(
        uv.x * 2.0 - 1.0,
        (1.0 - uv.y) * 2.0 - 1.0,
        rawDepth);
}

float GetLinearDepth(float rawDepth)
{
    return rcp(max(rawDepth, 1e-4));
}

float GetLuma(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

#define ps_CharacterShadowLift 0.0
#define ps_EquipmentShadowLift 0.0
#define ps_FaceShadowLift 0.0
#define ps_GIBentNormalBlend 0.7
#define ps_GICompositeIntensity 0.75
#define ps_GICompositeUpscaleSensitivity 0.025
#define ps_GICoolTintB 1.1
#define ps_GICoolTintG 0.85
#define ps_GICoolTintR 0.7
#define ps_GIDebugMode 0
#define ps_GIDenoiseStep 0.75
#define ps_GIDenoiseSigmaLuma 1.5
#define ps_GIDenoiseSigmaNormal 1.1
#define ps_GIDepthSensitivity 0.07
#define ps_GIDirectionalStrength 0.65
#define ps_GIFireflyClamp 4.0
#define ps_GIIndirectSpecularStrength 0.25
#define ps_GIIntensity 0.45
#define ps_GIMaxDirectionalFocus 0.72
#define ps_GIMultiBounceStrength 0.35
#define ps_GIRayCount 2
#define ps_GIRayLength 420.0
#define ps_GIReceiverAlbedoStrength 1.0
#define ps_GIReceiverSkinReject 0.95
#define ps_GIReceiverSpecReject 0.85
#define ps_GISaturation 1.0
#define ps_GISkinReject 0.85
#define ps_GISkylightStrength 0.35
#define ps_GISpecularReject 0.9
#define ps_GIStepCount 14
#define ps_GIStylizePivot 0.18
#define ps_GIStylizeStrength 0.0
#define ps_GITemporalBlend 0.88
#define ps_GIThickness 24.0
#define ps_GIWarmTintB 0.7
#define ps_GIWarmTintG 0.9
#define ps_GIWarmTintR 1.05
#define ps_ToonSkinEnabled false
#define ps_TraceDirectDistance 1200.0
#define ps_TraceDirectStrength 1.25
#define ps_TraceLocalLightCount 2
#define ps_TraceMaxCorrection 0.85
#define ps_TraceShadowTemporal 0.85
#define ps_TraceSunAngularRadius 0.005
#define vu_SSAOEnabled true

#if SE_GI_VALIDATION_SHADER == 1
#include "visualSSRTGI.hlsl"
#elif SE_GI_VALIDATION_SHADER == 2
#include "visualSSRTGIComposite.hlsl"
#else
#error SE_GI_VALIDATION_SHADER must be 1 or 2.
#endif
