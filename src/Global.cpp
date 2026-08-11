#include <PCH.h>
#include <Global.h>

const char* defaultIni = R"(
; Enable/disable debugging of the plugin
; This is extensive debugging for the plugin
DEBUGGING=false
; Master kill switch for shader replacements and custom passes.
SHADERENGINE_EFFECTS_ON=true
; Enables the validated OG deferred-light scope and directional-cascade patch.
; Traced deferred lighting is independently gated by ps_GIEnabled.
SHADOW_UPGRADE_ON=false
; End distance of the highest-detail directional-shadow cascade, in game units.
; Applied after game data is ready when SHADOW_UPGRADE_ON=true. Restart after
; changing it. Must remain below both 3000 and the live fDirShadowDistance.
DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE=1600.0
; Enable/disable custom resource view updates and bindings for replaced shaders
; This setting applies to all replaced shaders
; Shaders with #include "common.inc" have access to ingame data like FPS, Camera position and shader settings
CUSTOMBUFFER_ON=true
; --- SHADOW STATIC CACHE (behavioral, experimental) ---
; Directional sun splits up to the live iDirShadowSplits count. Builds
; precombine-only static depth caches, restores depth on cache hits, then
; replays dynamic/unknown casters.
; Requires slot-safe DSV access; otherwise it falls back to vanilla rendering.
SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON=false
; --- COMMAND BUFFER REPLAY ---
; Experimental CPU optimization: during command-buffer replay, suppress redundant
; SRV and input-assembly binds by tracking state and forwarding only changes.
COMMAND_BUFFER_REPLAY_DEDUPE_SRV=false
; Custom resource view slot in shader (beyond what the game uses, default t31)
CUSTOMBUFFER_SLOT=31
; Per-draw classification tag resource view slot
DRAWTAG_SLOT=26
; Race group definitions exposed to shaders through GFXDrawTag[0].raceGroupMask.
; Format: RACE_GROUP_N=PluginName.esm|FormID,... where N is 0-31.
; Group 0 defaults to Fallout 4 vanilla humanoid races for toon exclusion.
RACE_GROUP_0=Fallout4.esm|0x13746,Fallout4.esm|0x1A009,Fallout4.esm|0xEAFB6,Fallout4.esm|0x11D83F,Fallout4.esm|0x11EB96,Fallout4.esm|0x2261A4,Fallout4.esm|0x10BD65,Fallout4.esm|0xE8D09
RACE_GROUP_1=Fallout4.esm|0x1A009
; G-buffer normal target index (into RendererData.renderTargets[]).
; Default 20 = kGbufferNormal for the OG runtime; override only if the
; deferred renderer layout differs on your runtime. -1 disables the
; `gbufferNormal` built-in input for customPass blocks.
NORMAL_BUFFER_INDEX=20
; Packed shader settings resource view slots
MODULAR_FLOATS_SLOT=29
MODULAR_INTS_SLOT=28
MODULAR_BOOLS_SLOT=27
; --- SHADER SETTINGS ---
; Enable/disable settings menu
SHADERSETTINGS_ON=false
; MENU Hotkey Unused at the moment and defaults to the END key (ImGui hardcoded ENUM)
SHADERSETTINGS_MENUHOTKEY=VK_END
; Settings save Hotkey Unused at the moment and defaults to the HOME key (ImGui hardcoded ENUM)
SHADERSETTINGS_SAVEHOTKEY=VK_HOME
; Menu width in pixels
SHADERSETTINGS_WIDTH=600
; Menu height in pixels
SHADERSETTINGS_HEIGHT=300
; Shader settings menu opacity (0.0 - 1.0)
SHADERSETTINGS_OPACITY=0.75
; --- DEVELOPMENT SETTINGS ---
; Enable/disable development features like dump/log shaders
; This also enables file watchers for the INI and hlsl files
DEVELOPMENT=false
; Enable/disable Development GUI ingame (it may render the UI window on ingame textures like NPC foreheads :))
; Without this enabled, the entire ImGui initialization is skipped
DEVGUI_ON=false
; Development GUI Width
DEVGUI_WIDTH=600
; Development GUI Height
DEVGUI_HEIGHT=300
; Development GUI opacity (0.0 - 1.0)
DEVGUI_OPACITY=0.75

LIGHT_SORTER_MODE=off

; Folder structure
; /F4SE/Plugins/ShaderEngine.ini - main configuration file for shader replacement rules
; /F4SE/Plugins/ShaderEngineDumps/<ShaderDefinition ID>/ - folder with dumped original shaders for analysis
; /F4SE/Plugins/ShaderEngine/<ShaderDefinition>/ - folder for replacement shaders
; /F4SE/Plugins/ShaderEngine/<ShaderDefinition>/Shader.ini - settings for the replacement shader, see below for example format
; /F4SE/Plugins/ShaderEngine/<ShaderDefinition>/Values.ini - settings for shader values, see below for example format
; /F4SE/Plugins/ShaderEngine/<ShaderDefinition>/<Shadername>.ps.hlsl - example replacement pixel shader in HLSL
; /F4SE/Plugins/ShaderEngine/<ShaderDefinition>/<Shadername>.vs.hlsl - example replacement vertex shader in HLSL

;Dimensions:
; D3D11_SRV_DIMENSION_TEXTURE1D = 3
; D3D11_SRV_DIMENSION_TEXTURE2D = 4
; D3D11_SRV_DIMENSION_TEXTURE2DMS = 6
; D3D11_SRV_DIMENSION_TEXTURE3D = 7
; D3D11_SRV_DIMENSION_TEXTURECUBE = 8
; D3D11_SRV_DIMENSION_TEXTURE1DARRAY = 4
; D3D11_SRV_DIMENSION_TEXTURE2DARRAY = 5
; D3D11_SRV_DIMENSION_TEXTURECUBEARRAY = 11

; Example shader definition in /F4SE/Plugins/ShaderEngine/<ShaderDefinitionName>/Shader.ini
;[loadingScreen]             ; unique ShaderDefinition ID for this replacement rule, whitespace is removed for parsing
;active=true                 ; whether this shader replacement rule is active
;shadowUpgrade=false         ; group this rule under the SHADOW_UPGRADE_ON feature switch
;priority=0                  ; priority of this rule for matching when multiple rules could apply (lower number = higher priority)
;type=ps                     ; shader type (vs=vertex, ps=pixel) defaults to ps if not specified
;shaderUID=PS1A2B3C4DI3O2    ; Unique identifier for the shader, used for matching and logging, can be more than one comma separated values
;hash=0x8D118ECC             ; vector of exact match of expected hash of the original shader bytecode for detection (can be obtained from logs or dumps)
;asmHash=0x12345678          ; vector of exact match of expected hash of the original shader assembly for detection (can be obtained from logs or dumps)
;fxpShaderType=4             ; exact native BSShader family (OG: 4=DFLight, 6=DFComposite)
;fxpPixelShaderID=0x00200220 ; exact permutation key selected from Shaders011.fxp; comma separated values are accepted
;size=(>1024), (<4096)       ; size definitions for the shader bytecode, can specify multiple separated by commas for multiple acceptable sizes (e.g. (512), (>1024), (<4096)), or leave empty to ignore size check
;buffersize=368@2            ; exact match of expected buffer size for the shader (size@slot), can specify multiple separated by commas for multiple buffers
;textures=2,4                ; list of texture register slots used by the shader (e.g. 0,1,2 or 4,5 for t0,t1,t2 or t4,t5)
;textureDimensions=4@2,8@4   ; texture dimension @ slot (e.g. 4@2 = Texture2D at t2, 8@4 = TextureCube at t4). Dimensions: 1D=3, 2D=4, 2DMS=6, 3D=7, Cube=8, 2DArray=5, CubeArray=11
;textureSlotMask=0x14        ; bitmask for required texture slots (bit i=1 if ti required; 0x14 = t2,t4)
;textureDimensionMask=0x110  ; bitmask for texture dimensions (bit i=1 if dimension i used; 0x110 = Texture2D(4) + Cube(8))
;inputTextureCount=(>0)      ; input texture count definitions for the shader, can specify multiple separated by commas (e.g. (0), (>0), (<4)), or leave empty to ignore input texture count check
;inputcount=(>7)             ; non texture inputcount definitions for the shader, can specify multiple separated by commas (e.g. (8), (>4), (<16)), or leave empty to ignore input count check
;inputMask=0x0               ; match of the bitmask for required input registers (bit i is 1 if input register i is required)
;outputcount=(1)             ; outputcount definitions for the shader, can specify multiple separated by commas (e.g. (1), (>0), (<4)), or leave empty to ignore output count check
;outputMask=0x1              ; match of the bitmask for required output registers (bit i is 1 if output register o[i] is required)
;shader=ShaderEngineLS.hlsl    ; the replacement shader file name in the shader definition folder, CANNOT have white spaces in the filename and must be a .hlsl file
;log=true                    ; whether to log shader detection and reflection details to the F4SE logs for this shader replacement rule
;dump=true                   ; whether to dump the original shader for analysis to the ShaderEngineDumps folder for this shader replacement rule (existing dumps files will not be overwritten, but skipped)
;[/loadingScreen]

; Adding #include "common.inc" to the replacement shader gives access to:
;    float    GFXInjected[0].g_Time
;    float    GFXInjected[0].g_Delta;
;    float    GFXInjected[0].g_DayCycle;
;    float    GFXInjected[0].g_Frame;
;    float    GFXInjected[0].g_FPS;
;    float    GFXInjected[0].g_ResX;
;    float    GFXInjected[0].g_ResY;
;    float    GFXInjected[0].g_MouseX;
;    float    GFXInjected[0].g_MouseY;
;    float    GFXInjected[0].g_WindSpeed; // updated every 30 frames
;    float    GFXInjected[0].g_WindAngle; // updated every 30 frames
;    float    GFXInjected[0].g_WindTurb; // updated every 30 frames
;    float    GFXInjected[0].g_VpLeft;
;    float    GFXInjected[0].g_VpTop;
;    float    GFXInjected[0].g_VpWidth;
;    float    GFXInjected[0].g_VpHeight;
;    float3   GFXInjected[0].g_CameraPos;
;    float    GFXInjected[0].g_RadExp; // rad dmg taken over 30 frames
;    float3   GFXInjected[0].g_ViewDir;
;    float    GFXInjected[0].g_HealthPerc; // updated every 30 frames
;    float4   GFXInjected[0].g_InvProjRow0;
;    float4   GFXInjected[0].g_InvProjRow1;
;    float4   GFXInjected[0].g_InvProjRow2;
;    float4   GFXInjected[0].g_InvProjRow3;
;    float4   GFXInjected[0].g_InvViewRow0;
;    float4   GFXInjected[0].g_InvViewRow1;
;    float4   GFXInjected[0].g_InvViewRow2;
;    float4   GFXInjected[0].g_InvViewRow3;
;    float    GFXInjected[0].g_Random; // random value updated every frame
;    float    GFXInjected[0].g_Combat; // updated every 30 frames
;    float    GFXInjected[0].g_Interior; // updated every 30 frames
;    float4   GFXInjected[0].g_ViewProjRow0;
;    float4   GFXInjected[0].g_ViewProjRow1;
;    float4   GFXInjected[0].g_ViewProjRow2;
;    float4   GFXInjected[0].g_ViewProjRow3;
;    float4   GFXInjected[0].g_PrevViewProjRow0; // previous frame's VP rows
;    float4   GFXInjected[0].g_PrevViewProjRow1; // (zero on the very first frame;
;    float4   GFXInjected[0].g_PrevViewProjRow2; //  shaders should detect this and
;    float4   GFXInjected[0].g_PrevViewProjRow3; //  skip temporal reuse for one frame)
;    float    GFXInjected[0].g_TimeOfDay; // game time in 24-hour format
;    float    GFXInjected[0].g_WeatherTransition;
;    uint     GFXInjected[0].g_CurrentWeatherID;
;    uint     GFXInjected[0].g_OutgoingWeatherID;
;    uint     GFXInjected[0].g_CurrentLocationID;
;    uint     GFXInjected[0].g_WorldSpaceID;
;    uint     GFXInjected[0].g_SkyMode;
;    int      GFXInjected[0].g_CurrentWeatherClass;
;    int      GFXInjected[0].g_OutgoingWeatherClass;
;    float4   GFXInjected[0].g_CameraLocalRow0;
;    float4   GFXInjected[0].g_CameraLocalRow1;
;    float4   GFXInjected[0].g_CameraLocalRow2;
;    float4   GFXInjected[0].g_CameraLocalRow3;
;    float4   GFXInjected[0].g_CameraWorldRow0;
;    float4   GFXInjected[0].g_CameraWorldRow1;
;    float4   GFXInjected[0].g_CameraWorldRow2;
;    float4   GFXInjected[0].g_CameraWorldRow3;
;    float4   GFXInjected[0].g_CameraPreviousWorldRow0;
;    float4   GFXInjected[0].g_CameraPreviousWorldRow1;
;    float4   GFXInjected[0].g_CameraPreviousWorldRow2;
;    float4   GFXInjected[0].g_CameraPreviousWorldRow3;
;    float4   GFXInjected[0].g_FogDistances0; // x=near, y=far, z=waterNear, w=waterFar
;    float4   GFXInjected[0].g_FogDistances1; // x=heightMid, y=heightRange, z=farHeightMid, w=farHeightRange
;    float4   GFXInjected[0].g_FogParams; // x=fogHeight, y=fogPower, z=fogClamp, w=fogHighDensityScale
;    float4   GFXInjected[0].g_FogColor; // xyz=reserved fog RGB, currently zero; w=reserved
;    float    GFXInjected[0].g_SunR; // dominant stylized light red
;    float    GFXInjected[0].g_SunG; // dominant stylized light green
;    float    GFXInjected[0].g_SunB; // dominant stylized light blue
;    float    GFXInjected[0].g_SunDirX; // dominant stylized light direction X
;    float    GFXInjected[0].g_SunDirY; // dominant stylized light direction Y
;    float    GFXInjected[0].g_SunDirZ; // dominant stylized light direction Z
;    float    GFXInjected[0].g_SunValid; // 0=no dominant light, exterior=1, interior=directional fade
;    float4   GFXInjected[0].g_SH_R; // L1 SH for R channel (.xyz=dir bands, .w=DC). From RE::Sky 6-axis ambient cube.
;    float4   GFXInjected[0].g_SH_G; // L1 SH for G channel.
;    float4   GFXInjected[0].g_SH_B; // L1 SH for B channel.
;    float4   GFXInjected[0].g_CurrentCameraPositionAdjust; // absolute BGS camera translation for the current frame
;    float4   GFXInjected[0].g_PreviousCameraPositionAdjust; // translation paired with g_PrevViewProjRow*
;    float4   GFXInjected[0].g_RenderInfo; // xy=logical render extent, zw=display extent
;    float4   GFXInjected[0].g_WorldSH_R; // world-space L1 ambient cube for R; .xyz=directional bands, .w=DC
;    float4   GFXInjected[0].g_WorldSH_G; // world-space L1 ambient cube for G
;    float4   GFXInjected[0].g_WorldSH_B; // world-space L1 ambient cube for B

; Settings for shaders can be defined in the Values.ini file in the shader definition folder
; Globals are at the top of the menu, while locals are grouped with other values of the shader definition
;[global]
;id=g_TAAEnabled          ; the name of the variable in the shader to set, e.g. g_TAAEnabled
;label="TAA Enabled"      ; the label to show in the menu for this setting
;type=bool                ; the type of the variable (bool, int, float)
;value=true               ; the default value to set (true/false for bool, numeric value for int and float)
;[/global]
;[local]
;id=g_SomeFloatValue      ; the name of the variable in the shader to set, e.g. g_SomeFloatValue
;label="Some Float Value" ; the label to show in the menu for this setting
;group=Example            ; optional group name to organize this setting in the menu
;tooltip=Hover help text   ; optional wrapped help shown while hovering the control
;type=float               ; the type of the variable (bool, int, float)
;value=0.5                ; the default value to set (true/false for bool, numeric value for int and float)
;min=0.0                  ; optional minimum value for float and int types
;max=1.0                  ; optional maximum value for float and int types
;step=0.1                 ; optional step value for float and int types
;[/local]
;[local]
;id=g_SomeIntValue        ; the name of the variable in the shader to set, e.g. g_SomeIntValue
;label="Some Int Value"   ; the label to show in the menu for this setting
;type=int                 ; the type of the variable (bool, int, float)
;value=5                  ; the default value to set (true/false for bool, numeric value for int and float)
;min=0                    ; optional minimum value for float and int types
;max=10                   ; optional maximum value for float and int types
;step=1                   ; optional step value for float and int types
;[/local]
)";

const char* flashPixelShaderHLSL = R"(
// Pixel Shader that outputs a bright neon green color for testing shader replacement.
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 extra0 : TEXCOORD0;
    float4 extra1 : TEXCOORD1;
};
float4 main(PS_INPUT input) : SV_Target {
    // High-intensity Neon Green (Values > 1.0 trigger Bloom/HDR)
    // Change this to float3(10.0, 0.0, 10.0) for Magenta
    float3 neonColor = float3(0.0, 10.0, 0.5); 
    return float4(neonColor, 1.0);
}
)";

bool SupportsVoxelTypedUavLoads()
{
    auto* device = g_rendererData ? g_rendererData->device : nullptr;
    if (!device) {
        return false;
    }

    struct FormatSupport2Data {
        REX::W32::DXGI_FORMAT inFormat;
        std::uint32_t outFormatSupport2;
    };

    static std::mutex cacheMutex;
    static REX::W32::ID3D11Device* cachedDevice = nullptr;
    static bool cachedResult = false;
    std::lock_guard lock(cacheMutex);
    if (cachedDevice == device) {
        return cachedResult;
    }

    const auto supportsTypedLoad = [device](REX::W32::DXGI_FORMAT format) {
        FormatSupport2Data support{ format, 0 };
        const auto hr = device->CheckFeatureSupport(
            REX::W32::D3D11_FEATURE_FORMAT_SUPPORT2,
            &support,
            sizeof(support));
        return REX::W32::SUCCESS(hr) &&
               (support.outFormatSupport2 &
                REX::W32::D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
    };

    cachedResult = supportsTypedLoad(REX::W32::DXGI_FORMAT_R16_FLOAT) &&
                   supportsTypedLoad(REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT);
    cachedDevice = device;
    REX::INFO("Voxel GI: R16/RGBA16 typed UAV loads {}",
        cachedResult ? "supported" : "unsupported; using R32 volume formats");
    return cachedResult;
}

std::string GetCommonShaderHeaderHLSLTop()
{
    std::string header = SupportsVoxelTypedUavLoads()
        ? "#define VX_TYPED_UAV_LOADS 1\n"
        : "#define VX_TYPED_UAV_LOADS 0\n";
    header += R"(
        // Data passed from the plugin as resource view
        struct GFXBoosterAccessData
        {
            // Block 0 (Bytes 0-15)
            float    g_Time;
            float    g_Delta;
            float    g_DayCycle;
            float    g_Frame;

            // Block 1 (Bytes 16-31)
            float    g_FPS;
            float    g_ResX;
            float    g_ResY;
            float    g_MouseX;

            // Block 2 (Bytes 32-47)
            float    g_MouseY;
            float    g_WindSpeed;
            float    g_WindAngle;
            float    g_WindTurb;

            // Block 3 (Bytes 48-63)
            float    g_VpLeft;
            float    g_VpTop;
            float    g_VpWidth;
            float    g_VpHeight;

            // Block 4 (Bytes 64-79)
            float3   g_CameraPos;
            float    g_RadExp;

            // Block 5 (Bytes 80-95)
            float3   g_ViewDir;
            float    g_HealthPerc;

            // Block 6 (Bytes 96-159)
            float4   g_InvProjRow0;
            float4   g_InvProjRow1;
            float4   g_InvProjRow2;
            float4   g_InvProjRow3;

            float4   g_InvViewRow0;
            float4   g_InvViewRow1;
            float4   g_InvViewRow2;
            float4   g_InvViewRow3;

            // Block 7
            float    g_Random;
            float    g_Combat;
            float    g_Interior;
            float    _padding;

            // Block 8 (Bytes 176-239)
            float4   g_ViewProjRow0;
            float4   g_ViewProjRow1;
            float4   g_ViewProjRow2;
            float4   g_ViewProjRow3;

            // Block 8b: Previous-frame view-projection (for temporal reprojection).
            float4   g_PrevViewProjRow0;
            float4   g_PrevViewProjRow1;
            float4   g_PrevViewProjRow2;
            float4   g_PrevViewProjRow3;

            // ENB Helper style game state
            float g_TimeOfDay;
            float g_WeatherTransition;
            uint  g_CurrentWeatherID;
            uint  g_OutgoingWeatherID;

            uint  g_CurrentLocationID;
            uint  g_WorldSpaceID;
            uint  g_SkyMode;
            int   g_CurrentWeatherClass;

            int   g_OutgoingWeatherClass;
            float _enbPadding0;
            float _enbPadding1;
            float _enbPadding2;

            float4 g_CameraLocalRow0;
            float4 g_CameraLocalRow1;
            float4 g_CameraLocalRow2;
            float4 g_CameraLocalRow3;

            float4 g_CameraWorldRow0;
            float4 g_CameraWorldRow1;
            float4 g_CameraWorldRow2;
            float4 g_CameraWorldRow3;

            float4 g_CameraPreviousWorldRow0;
            float4 g_CameraPreviousWorldRow1;
            float4 g_CameraPreviousWorldRow2;
            float4 g_CameraPreviousWorldRow3;

            // Live engine fog state from RE::Sky (blended through weather/cell).
            float4 g_FogDistances0;  // x=near, y=far, z=waterNear, w=waterFar
            float4 g_FogDistances1;  // x=heightMid, y=heightRange, z=farHeightMid, w=farHeightRange
            float4 g_FogParams;      // x=fogHeight, y=fogPower, z=fogClamp, w=fogHighDensityScale
            float4 g_FogColor;       // xyz=blended RGB (0 until per-weather blend lands), w=reserved

            float  g_SunR;
            float  g_SunG;
            float  g_SunB;
            float  g_SunDirX;

            float  g_SunDirY;
            float  g_SunDirZ;
            float  g_SunValid;
            float  _sunPadding;

            // L1 SH coefficients per color channel, computed plugin-side from
            // RE::Sky::directionalAmbientColorsA (6-axis directional ambient
            // cube, weather/cell-blended). Packing:
            //   .x = X+ - X- band, .y = Y+ - Y-, .z = Z+ - Z-, .w = DC mean.
            // Linear domain — evaluate as float3(dot(g_SH_R, float4(N,1)),
            //                                   dot(g_SH_G, float4(N,1)),
            //                                   dot(g_SH_B, float4(N,1))).
            float4 g_SH_R;
            float4 g_SH_G;
            float4 g_SH_B;

            // Exact BSGraphics::CameraStateData translation paired with the
            // current and previous view-projection matrices. BGS keeps this
            // translation outside its rotation-only view matrix.
            float4 g_CurrentCameraPositionAdjust;
            float4 g_PreviousCameraPositionAdjust;

            // Logical projection extent and physical display extent. External
            // upscalers may keep some engine resources display-sized while
            // rendering the G-buffer through reduced proxy allocations.
            float4 g_RenderInfo; // xy=render, zw=display

            // World-space L1 coefficients from the same blended ambient cube
            // as g_SH. These fields are appended to preserve the offsets of
            // the established structured-buffer ABI. World-space tracing
            // passes evaluate them directly with float4(worldDirection, 1).
            float4 g_WorldSH_R;
            float4 g_WorldSH_G;
            float4 g_WorldSH_B;
        };

        struct DrawTagData
        {
            float materialTag;
            float isHead;
            uint raceGroupMask;
            uint raceFlags;
        };

        #define GFX_RACE_FLAG_RESOLVED 0x00000001u
        #define GFX_RACE_GROUP_0       0x00000001u
        #define GFX_RACE_GROUP_1       0x00000002u
        #define GFX_RACE_GROUP_2       0x00000004u
        #define GFX_RACE_GROUP_3       0x00000008u
        #define GFX_RACE_GROUP_4       0x00000010u
        #define GFX_RACE_GROUP_5       0x00000020u
        #define GFX_RACE_GROUP_6       0x00000040u
        #define GFX_RACE_GROUP_7       0x00000080u
        #define GFX_RACE_GROUP_8       0x00000100u
        #define GFX_RACE_GROUP_9       0x00000200u
        #define GFX_RACE_GROUP_10      0x00000400u
        #define GFX_RACE_GROUP_11      0x00000800u
        #define GFX_RACE_GROUP_12      0x00001000u
        #define GFX_RACE_GROUP_13      0x00002000u
        #define GFX_RACE_GROUP_14      0x00004000u
        #define GFX_RACE_GROUP_15      0x00008000u
        #define GFX_RACE_GROUP_16      0x00010000u
        #define GFX_RACE_GROUP_17      0x00020000u
        #define GFX_RACE_GROUP_18      0x00040000u
        #define GFX_RACE_GROUP_19      0x00080000u
        #define GFX_RACE_GROUP_20      0x00100000u
        #define GFX_RACE_GROUP_21      0x00200000u
        #define GFX_RACE_GROUP_22      0x00400000u
        #define GFX_RACE_GROUP_23      0x00800000u
        #define GFX_RACE_GROUP_24      0x01000000u
        #define GFX_RACE_GROUP_25      0x02000000u
        #define GFX_RACE_GROUP_26      0x04000000u
        #define GFX_RACE_GROUP_27      0x08000000u
        #define GFX_RACE_GROUP_28      0x10000000u
        #define GFX_RACE_GROUP_29      0x20000000u
        #define GFX_RACE_GROUP_30      0x40000000u
        #define GFX_RACE_GROUP_31      0x80000000u
    )";
    return header;
}

// Here will be the dynamic Shader Settings values defined

std::string GetCommonShaderHeaderHLSLBottom()
{
    return std::string(R"(
        StructuredBuffer<GFXBoosterAccessData> GFXInjected : register(t)") +
        // Adding the custom buffer slot from the INI
        // The line will produce: StructuredBuffer<GFXBoosterAccessData> GFXInjected : register(t31);
        std::to_string(CUSTOMBUFFER_SLOT) +
        std::string(R"();
        StructuredBuffer<DrawTagData> GFXDrawTag : register(t)") +
        std::to_string(DRAWTAG_SLOT) +
        std::string(R"();
        StructuredBuffer<float4> GFXModularFloats : register(t)") +
        std::to_string(MODULAR_FLOATS_SLOT) +
        std::string(R"();
        StructuredBuffer<int4> GFXModularInts : register(t)") +
        std::to_string(MODULAR_INTS_SLOT) +
        std::string(R"();
        StructuredBuffer<uint4> GFXModularBools : register(t)") +
        std::to_string(MODULAR_BOOLS_SLOT) +
        std::string(R"();

        // --- Coordinate Space Helpers ---

        // Transforms screen UV and raw depth into world-space coordinates.
        float3 ReconstructWorldPos(float2 uv, float rawDepth)
        {
            float4 clipPos;
            clipPos.x = uv.x * 2.0 - 1.0;
            clipPos.y = (1.0 - uv.y) * 2.0 - 1.0; 
            clipPos.z = rawDepth;
            clipPos.w = 1.0;
            float4x4 invProj = float4x4(
                GFXInjected[0].g_InvProjRow0,
                GFXInjected[0].g_InvProjRow1,
                GFXInjected[0].g_InvProjRow2,
                GFXInjected[0].g_InvProjRow3
            );
            float4 viewPos = mul(clipPos, invProj);
            viewPos.xyz *= (abs(viewPos.w) > 1e-6) ? rcp(viewPos.w) : 1.0;
            viewPos.w = 1.0;

            float4x4 invView = float4x4(
                GFXInjected[0].g_InvViewRow0,
                GFXInjected[0].g_InvViewRow1,
                GFXInjected[0].g_InvViewRow2,
                GFXInjected[0].g_InvViewRow3
            );
            float4 worldPos = mul(viewPos, invView);
            return worldPos.xyz * ((abs(worldPos.w) > 1e-6) ? rcp(worldPos.w) : 1.0);
        }

        // --- Color Conversion Helpers ---

        // Generates an RGB spectrum color based on a 0.0-1.0 hue input.
        float3 HueToRGB(float h)
        {
            float r = abs(h * 6.0 - 3.0) - 1.0;
            float g = 2.0 - abs(h * 6.0 - 2.0);
            float b = 2.0 - abs(h * 6.0 - 4.0);
            return saturate(float3(r, g, b));
        }

        // Returns the perceptual brightness of an RGB color using standard luminance weights.
        float GetLuma(float3 rgb)
        {
            return dot(rgb, float3(0.299, 0.587, 0.114));
        }

        // Performs a three-way linear interpolation across a color gradient (a to b to c).
        float3 Lerp3(float3 a, float3 b, float3 c, float t)
        {
            if (t < 0.5) return lerp(a, b, t * 2.0);
            return lerp(b, c, (t - 0.5) * 2.0);
        }

        // --- Other helpers ---

        // ditherValue: 0 to 100 (0 = fully opaque, 100 = fully transparent)
        void transparentDither(uint2 pixelPos, float transparency)
        {
            // A simple 2x2 checkerboard logic
            // This creates the "grain" look
            bool checker = ((pixelPos.x + pixelPos.y) % 2 == 0);
            
            // For 50% transparency (your sweet spot)
            if (transparency >= 50.0) {
                if (checker) discard;
            }
            
            // For higher transparency (like your 97% ghost shader)
            // We add an extra skip to thin out the remaining 50%
            if (transparency > 75.0) {
                if ((pixelPos.x % 2 == 0) || (pixelPos.y % 2 == 0)) discard;
            }
        }

        float2 GetWindDir()
        {
            float s, c;
            // Using sincos to transform your scalar angle into a 2D vector
            sincos(GFXInjected[0].g_WindAngle, s, c);
            return float2(c, s);
        }

        float2 GetWindFlow(float speedMult)
        {
            return GetWindDir() * (GFXInjected[0].g_Time * GFXInjected[0].g_WindSpeed * speedMult);
        }

        // --- Math Helpers ---

        // Produces a static, deterministic pseudo-random value based strictly on UV coordinates.
        float RandomHash(float2 uv)
        {
            return frac(sin(dot(uv, float2(12.9898, 78.233))) * 43758.5453);
        }

        // Produces a dynamic pseudo-random value that changes every frame using the injected random seed.
        float RandomTemporal(float2 uv)
        {
            return frac(sin(dot(uv + GFXInjected[0].g_Random, float2(12.9898, 78.233))) * 43758.5453);
        }

        // Maps a numeric value from an input range [min1, max1] to an output range [min2, max2].
        float Remap(float value, float min1, float max1, float min2, float max2)
        {
            return min2 + (value - min1) * (max2 - min2) / (max1 - min1);
        }

        // Converts a non-linear raw depth buffer value into linear eye depth in game units.
        float GetLinearDepth(float rawDepth) {
            // We reconstruct a point at the center of the screen at the given depth
            float4 clipPos = float4(0, 0, rawDepth, 1.0);
            float4x4 invProj = float4x4(
                GFXInjected[0].g_InvProjRow0,
                GFXInjected[0].g_InvProjRow1,
                GFXInjected[0].g_InvProjRow2,
                GFXInjected[0].g_InvProjRow3
            );
            float4 viewPos = mul(clipPos, invProj);
            viewPos.xyz *= (abs(viewPos.w) > 1e-6) ? rcp(viewPos.w) : 1.0;
            return abs(viewPos.z); 
        }

        // Calculates UV offsets to simulate surface depth/relief without requiring extra geometry.
        float2 ApplyParallax(float2 uv, float3 viewDirTS, float3 worldPos, float scale, Texture2D heightMap, SamplerState sam) {
            // Distance Culling
            float dist = distance(worldPos, GFXInjected[0].g_CameraPos);
            if (dist > 2000.0) return uv; 

            // Initial Height
            float h = heightMap.SampleLevel(sam, uv, 0.5).y;
            
            // Anchor the scale to the angle
            // We use (1 - viewDirTS.z) to make sure top-down views have ZERO shift.
            float angleFactor = saturate(1.0 - viewDirTS.z);
            float finalScale = scale * angleFactor * saturate((h - 0.2) / 0.5);

            // THE PROJECTED OFFSET
            // We multiply by xy and ignore the division.
            float2 maxOffset = viewDirTS.xy * finalScale;

            // The Search Loop
            const float numSteps = 12.0;
            float2 uvStep = maxOffset / numSteps;
            float stepSize = 1.0 / numSteps;
            
            float2 currentUV = uv;
            float currentLayerDepth = 0.0;
            float prevHeight = h;

            [loop]
            for(int i = 0; i < 12; i++) {
                // Sample at Mip 1.0 to smoothen the turret edges
                float currentHeight = heightMap.SampleLevel(sam, currentUV, 1.0).y;
                if(currentLayerDepth < currentHeight) {
                    prevHeight = currentHeight;
                    currentUV -= uvStep;
                    currentLayerDepth += stepSize;
                } else {
                    float nextHeight = currentHeight;
                    float prevLayerDepth = currentLayerDepth - stepSize;
                    float weight = (currentLayerDepth - nextHeight) / (abs((nextHeight - prevHeight) + (prevLayerDepth - currentLayerDepth)) + 1e-6);
                    return lerp(currentUV, currentUV + uvStep, weight);
                }
            }

            return currentUV;
        }
        )");
}

// --- Compiled-shader cache implementation -------------------------------

extern std::filesystem::path g_shaderFolderPath;  // defined in main.cpp

namespace {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime  = 0x100000001b3ULL;

inline uint64_t FnvUpdate(uint64_t h, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= kFnvPrime; }
    return h;
}

std::filesystem::path GetCacheDir() {
    // Lives next to the per-shader folders, hidden via the leading dot so it
    // doesn't get mistaken for a definition folder by GetSubdirectories.
    std::filesystem::path d = g_shaderFolderPath / ".cache";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}

// --- Memoized include state ---------------------------------------------
//
// Both the include-dir hash (one FNV pass over every file in the include dir,
// done once per ComputeKey) and the include-file contents (read via ifstream
// on every #include from D3DCompile) used to be recomputed on every shader
// compile. With N matched shaders bound back-to-back at world load that's
// N copies of the same disk traffic. Memoize both, drop the memo on Shader.ini
// reload (where the include-dir contents may have changed).

std::mutex                                            g_includeMemoMutex;
bool                                                  g_includeHashCached = false;
uint64_t                                              g_includeHashValue  = 0;
std::unordered_map<std::string, std::vector<char>>    g_includeContentCache;

// Hash every regular file inside g_commonShaderHeaderPath (filename + body)
// in sorted order so any change to a shared include invalidates dependent
// caches without us needing to track per-shader include graphs.
uint64_t HashCommonIncludeDir() {
    {
        std::lock_guard lk(g_includeMemoMutex);
        if (g_includeHashCached) return g_includeHashValue;
    }
    if (g_commonShaderHeaderPath.empty()) return 0;
    std::error_code ec;
    if (!std::filesystem::exists(g_commonShaderHeaderPath, ec)) return 0;
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(g_commonShaderHeaderPath, ec)) {
        if (e.is_regular_file(ec)) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    uint64_t h = kFnvOffset;
    for (auto& f : files) {
        auto name = f.filename().string();
        h = FnvUpdate(h, name.data(), name.size());
        std::ifstream ifs(f, std::ios::binary);
        if (!ifs) continue;
        std::string body((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        h = FnvUpdate(h, body.data(), body.size());
    }
    {
        std::lock_guard lk(g_includeMemoMutex);
        g_includeHashValue  = h;
        g_includeHashCached = true;
    }
    return h;
}

}  // namespace

namespace ShaderCache {

std::string ComputeKey(const CompileInputs& inputs) {
    uint64_t h = kFnvOffset;
    h = FnvUpdate(h, inputs.assembledSource.data(), inputs.assembledSource.size());
    h = FnvUpdate(h, inputs.profile.data(),         inputs.profile.size());
    h = FnvUpdate(h, inputs.entry.data(),           inputs.entry.size());
    h = FnvUpdate(h, &inputs.flags,                 sizeof(inputs.flags));
    const uint64_t inc = HashCommonIncludeDir();
    h = FnvUpdate(h, &inc, sizeof(inc));
    return std::format("{:016x}", h);
}

bool TryLoad(const std::string& key, ID3DBlob** outBlob) {
    if (!outBlob) return false;
    *outBlob = nullptr;

    const auto path = GetCacheDir() / (key + ".bin");
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;

    char magic[4]{};
    f.read(magic, 4);
    if (!f.good() || std::memcmp(magic, "SHC1", 4) != 0) return false;

    uint32_t version = 0;
    f.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!f.good() || version != kFileFormatVersion) return false;

    uint64_t storedKey = 0;
    f.read(reinterpret_cast<char*>(&storedKey), sizeof(storedKey));
    if (!f.good()) return false;
    // Cross-check filename-encoded key vs in-header copy: catches accidental
    // file copies/renames between cache slots.
    uint64_t expectedKey = 0;
    try { expectedKey = std::stoull(key, nullptr, 16); } catch (...) { return false; }
    if (storedKey != expectedKey) {
        REX::WARN("ShaderCache::TryLoad: header/key mismatch in {}, ignoring", path.string());
        return false;
    }

    uint64_t blobSize = 0;
    f.read(reinterpret_cast<char*>(&blobSize), sizeof(blobSize));
    // Sanity cap: any compiled blob over 64 MiB is almost certainly corrupt.
    if (!f.good() || blobSize == 0 || blobSize > (64ull << 20)) return false;

    ID3DBlob* blob = nullptr;
    if (FAILED(D3DCreateBlob(static_cast<SIZE_T>(blobSize), &blob)) || !blob) return false;
    f.read(reinterpret_cast<char*>(blob->GetBufferPointer()), static_cast<std::streamsize>(blobSize));
    if (static_cast<uint64_t>(f.gcount()) != blobSize) {
        blob->Release();
        return false;
    }
    *outBlob = blob;
    return true;
}

void Store(const std::string& key, ID3DBlob* blob) {
    if (!blob) return;
    const auto dir = GetCacheDir();
    const auto tmp = dir / (key + ".bin.tmp");
    const auto fin = dir / (key + ".bin");

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.good()) {
            REX::WARN("ShaderCache::Store: cannot open {} for write", tmp.string());
            return;
        }
        f.write("SHC1", 4);
        const uint32_t version = kFileFormatVersion;
        f.write(reinterpret_cast<const char*>(&version), sizeof(version));
        uint64_t storedKey = 0;
        try { storedKey = std::stoull(key, nullptr, 16); } catch (...) { return; }
        f.write(reinterpret_cast<const char*>(&storedKey), sizeof(storedKey));
        const uint64_t blobSize = static_cast<uint64_t>(blob->GetBufferSize());
        f.write(reinterpret_cast<const char*>(&blobSize), sizeof(blobSize));
        f.write(reinterpret_cast<const char*>(blob->GetBufferPointer()),
                static_cast<std::streamsize>(blobSize));
        if (!f.good()) {
            REX::WARN("ShaderCache::Store: write failed for {}", tmp.string());
            std::error_code ec; std::filesystem::remove(tmp, ec);
            return;
        }
    }
    // Atomic publish via rename. If the target already exists (e.g. a parallel
    // miss raced and wrote first) replace it — both blobs are equally valid.
    std::error_code ec;
    std::filesystem::rename(tmp, fin, ec);
    if (ec) {
        std::filesystem::remove(fin, ec);
        std::filesystem::rename(tmp, fin, ec);
        if (ec) {
            REX::WARN("ShaderCache::Store: rename failed for {}: {}",
                      fin.string(), ec.message());
            std::filesystem::remove(tmp, ec);
        }
    }
}

void InvalidateIncludeMemo() {
    std::lock_guard lk(g_includeMemoMutex);
    g_includeHashCached = false;
    g_includeHashValue  = 0;
    g_includeContentCache.clear();
}

}  // namespace ShaderCache

// --- ShaderIncludeHandler implementation --------------------------------
//
// Reads each include file once into g_includeContentCache; subsequent
// resolutions of the same `#include` line (across shaders, across compile
// invocations) hand back a copy of the cached bytes instead of hitting disk.
// Cache is flushed by ShaderCache::InvalidateIncludeMemo on Shader.ini reload.

HRESULT __stdcall ShaderIncludeHandler::Open(D3D_INCLUDE_TYPE /*IncludeType*/,
                                             LPCSTR pFileName,
                                             LPCVOID /*pParentData*/,
                                             LPCVOID* ppData,
                                             UINT* pBytes) {
    if (!ppData || !pBytes || !pFileName) return E_FAIL;
    const std::string key = pFileName;

    // Hot path: serve a copy from the in-memory cache.
    {
        std::lock_guard lk(g_includeMemoMutex);
        auto it = g_includeContentCache.find(key);
        if (it != g_includeContentCache.end()) {
            const auto& src = it->second;
            char* out = new char[src.size()];
            std::memcpy(out, src.data(), src.size());
            *ppData = out;
            *pBytes = static_cast<UINT>(src.size());
            return S_OK;
        }
    }

    // Cold path: read from disk and populate the cache.
    const std::filesystem::path includePath = g_commonShaderHeaderPath / pFileName;
    std::ifstream file(includePath, std::ios::binary);
    if (!file.good()) {
        REX::WARN("ShaderIncludeHandler: Failed to open include file: {}", includePath.string());
        return E_FAIL;
    }
    file.seekg(0, std::ios::end);
    const size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<char> body(size);
    if (size > 0) file.read(body.data(), static_cast<std::streamsize>(size));
    file.close();

    // Hand caller a fresh allocation (they own it; Close will delete[] it).
    char* out = new char[size];
    std::memcpy(out, body.data(), size);
    *ppData = out;
    *pBytes = static_cast<UINT>(size);

    {
        std::lock_guard lk(g_includeMemoMutex);
        // Insert if still absent (another thread may have populated meanwhile).
        g_includeContentCache.try_emplace(key, std::move(body));
    }
    return S_OK;
}

HRESULT __stdcall ShaderIncludeHandler::Close(LPCVOID pData) {
    delete[] static_cast<const char*>(pData);
    return S_OK;
}

// --- IncludeDirWatcher implementation -----------------------------------

std::unique_ptr<IncludeDirWatcher> g_includeDirWatcher;

// Starts at 0; every ShaderDefinition starts at 0 too, so no reload is
// pending until the UI button bumps this.
std::atomic<std::uint64_t> g_manualShaderReloadGeneration{ 0 };

namespace {

std::unordered_map<std::string, std::filesystem::file_time_type>
SnapshotIncludeDir(const std::filesystem::path& dir) {
    std::unordered_map<std::string, std::filesystem::file_time_type> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) {
            out.emplace(e.path().filename().string(), e.last_write_time(ec));
        }
    }
    return out;
}

}  // namespace

IncludeDirWatcher::IncludeDirWatcher(std::filesystem::path d) : dir(std::move(d)) {
    snapshot = SnapshotIncludeDir(dir);
}

IncludeDirWatcher::~IncludeDirWatcher() {
    Stop();
}

void IncludeDirWatcher::Start() {
    running = true;
    worker = std::thread([this]() {
        std::unique_lock lock(stopMutex);
        while (running) {
            lock.unlock();
            try { Check(); } catch (...) {}
            lock.lock();
            stopCv.wait_for(lock, std::chrono::seconds(1), [this]{ return !running; });
        }
    });
}

void IncludeDirWatcher::Stop() {
    {
        std::lock_guard lock(stopMutex);
        running = false;
    }
    stopCv.notify_all();
    if (worker.joinable()) worker.join();
}

void IncludeDirWatcher::Check() {
    auto now = SnapshotIncludeDir(dir);
    if (now == snapshot) return;
    snapshot = std::move(now);
    ShaderCache::InvalidateIncludeMemo();
    std::size_t reloadCount = 0;
    {
        std::shared_lock lock(g_shaderDefinitions.mutex);
        for (auto* def : g_shaderDefinitions.definitions) {
            if (def && def->active && def->hlslFileWatcher) {
                def->hlslFileWatcher->RequestReload();
                ++reloadCount;
            }
        }
    }
    REX::INFO(
        "IncludeDirWatcher: detected change under '{}', invalidated include memo and marked {} replacement shader(s) for reload",
        dir.string(), reloadCount);
}

// --- PrecompileWorker implementation ------------------------------------

std::unique_ptr<PrecompileWorker> g_precompileWorker;

void PrecompileWorker::Start() {
    if (running.load(std::memory_order_acquire)) return;
    running.store(true, std::memory_order_release);
    worker = std::thread([this]() {
        // Don't pop jobs until the renderer device exists — compile work
        // would just fail without it, and we'd waste the queue. Re-check
        // periodically; this loop only runs at startup before the engine
        // has initialized D3D, typically <1s.
        while (running.load(std::memory_order_acquire) &&
               (!g_rendererData || !g_rendererData->device)) {
            std::unique_lock lk(mutex);
            cv.wait_for(lk, std::chrono::milliseconds(100),
                        [this]{ return !running.load(std::memory_order_acquire); });
        }

        while (true) {
            NamedJob nj;
            {
                std::unique_lock lk(mutex);
                cv.wait(lk, [this]{
                    return !running.load(std::memory_order_acquire) || !jobs.empty();
                });
                if (!running.load(std::memory_order_acquire)) return;
                nj = std::move(jobs.front());
                jobs.pop_front();
            }
            if (!nj.job) continue;
            REX::INFO("PrecompileWorker: compiling '{}'...", nj.name);
            // Exceptions are swallowed: we don't want one bad shader/pass
            // to kill the whole worker mid-queue.
            try { nj.job(); }
            catch (...) {
                REX::WARN("PrecompileWorker: job '{}' threw an exception", nj.name);
            }
        }
    });
}

void PrecompileWorker::Stop() {
    {
        std::lock_guard lk(mutex);
        running.store(false, std::memory_order_release);
        jobs.clear();
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
}

void PrecompileWorker::Enqueue(std::string name, Job job) {
    if (!job) return;
    {
        std::lock_guard lk(mutex);
        jobs.push_back({ std::move(name), std::move(job) });
    }
    cv.notify_one();
}
