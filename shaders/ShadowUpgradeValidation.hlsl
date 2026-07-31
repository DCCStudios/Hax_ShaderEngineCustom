// Compile-only harness for validating deployed replacement shaders with fxc.
// ShaderEngine normally prepends the full generated GFXInjected declaration
// and Values.ini accessors before compiling. This compact declaration plus
// command-line value defines supplies only what the deferred-light includes
// need to parse.
struct SE_ValidationGFXInjected
{
    float g_TimeOfDay;
    float g_DayCycle;
    float g_WeatherTransition;
    float g_Interior;
    int g_CurrentWeatherClass;
    int g_OutgoingWeatherClass;
    float4 g_SH_R;
    float4 g_SH_G;
    float4 g_SH_B;
};

StructuredBuffer<SE_ValidationGFXInjected> GFXInjected : register(t31);

#if SE_VALIDATION_SHADER == 1
#include "pixelDeferredLightOG.hlsl"
#elif SE_VALIDATION_SHADER == 2
#include "pixelShadowVisibilityOG.hlsl"
#elif SE_VALIDATION_SHADER == 3
#include "pixelDeferredLightLocalOG.hlsl"
#elif SE_VALIDATION_SHADER == 4
#include "pixelDeferredLightLocalProjectedOG.hlsl"
#elif SE_VALIDATION_SHADER == 5
#include "pixelDeferredLightLocalProjectedAltOG.hlsl"
#elif SE_VALIDATION_SHADER == 6
#include "pixelDeferredLightLocalShadowProjectedOG.hlsl"
#elif SE_VALIDATION_SHADER == 7
#include "pixelDeferredLightLocalSimpleOG.hlsl"
#elif SE_VALIDATION_SHADER == 8
#include "pixelDeferredLightLocalSimpleAltOG.hlsl"
#elif SE_VALIDATION_SHADER == 9
#include "pixelDeferredLightInteriorOG.hlsl"
#elif SE_VALIDATION_SHADER == 10
#include "pixelDeferredLightMainBF3C87EOG.hlsl"
#else
#error SE_VALIDATION_SHADER must select a deployed HLSL file.
#endif
