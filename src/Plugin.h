#pragma once
#include <Global.h>

// Forward declaration of HlslFileWatcher for use in ShaderDefinition
class HlslFileWatcher; 
// Forward declaration of g_pluginPath
extern std::filesystem::path g_pluginPath;
// Helper function to convert string to lowercase
std::string ToLower(const std::string& str);

// --- Hooks ---

// -- Structs ---

// Custom t# buffer structure for passing data to shaders
struct alignas(16) GFXBoosterAccessData
{
    float time;        //  0
    float delta;       
    float dayCycle;    
    float frame;       //  3

    float fps;         //  4
    float resX;        
    float resY;        
    float mouseX;      //  7

    float mouseY;      //  8
    float windSpeed;   
    float windAngle;   
    float windTurb;    // 11

    float vpLeft;      // 12
    float vpTop;       
    float vpWidth;     
    float vpHeight;    // 15

    float camX;        // 16
    float camY;        
    float camZ;        
    float pRadDmg;     // 19

    float viewDirX;    // 20
    float viewDirY;    
    float viewDirZ;    

    float pHealthPerc; // 23
    DirectX::XMFLOAT4 g_InvProjRow0; // 24
    DirectX::XMFLOAT4 g_InvProjRow1; // 25
    DirectX::XMFLOAT4 g_InvProjRow2; // 26
    DirectX::XMFLOAT4 g_InvProjRow3; // 27

    // Inverse view rows. ReconstructWorldPos needs these to lift
    // projection-space positions into world space instead of view space.
    DirectX::XMFLOAT4 g_InvViewRow0;
    DirectX::XMFLOAT4 g_InvViewRow1;
    DirectX::XMFLOAT4 g_InvViewRow2;
    DirectX::XMFLOAT4 g_InvViewRow3;

    float random;    // 28
    float  inCombat;     // 29
    float  inInterior;   // 30
    float _padding;   // 31

    // Forward view-projection rows (needed for SSR world ??clip reprojection)
    DirectX::XMFLOAT4 g_ViewProjRow0; // 32
    DirectX::XMFLOAT4 g_ViewProjRow1; // 33
    DirectX::XMFLOAT4 g_ViewProjRow2; // 34
    DirectX::XMFLOAT4 g_ViewProjRow3; // 35

    // Renderer-maintained previous unjittered view-projection rows from
    // BSGraphics::ViewData. Pair with g_PreviousCameraPositionAdjust because
    // BGS stores camera translation outside this matrix.
    DirectX::XMFLOAT4 g_PrevViewProjRow0;
    DirectX::XMFLOAT4 g_PrevViewProjRow1;
    DirectX::XMFLOAT4 g_PrevViewProjRow2;
    DirectX::XMFLOAT4 g_PrevViewProjRow3;

    float    timeOfDay;          // Game time in 24-hour format
    float    weatherTransition;  // Current weather transition percentage
    uint32_t currentWeatherID;
    uint32_t outgoingWeatherID;

    uint32_t currentLocationID;
    uint32_t worldSpaceID;
    uint32_t skyMode;
    int32_t  currentWeatherClass;

    int32_t  outgoingWeatherClass;
    float    waterHeight;       // absolute Z of the player cell's water plane, -1e9 = none
    float    cameraUnderwater;  // 1 when the camera is below that plane
    float    enbPadding2;

    DirectX::XMFLOAT4 cameraLocalRow0;
    DirectX::XMFLOAT4 cameraLocalRow1;
    DirectX::XMFLOAT4 cameraLocalRow2;
    DirectX::XMFLOAT4 cameraLocalRow3;

    DirectX::XMFLOAT4 cameraWorldRow0;
    DirectX::XMFLOAT4 cameraWorldRow1;
    DirectX::XMFLOAT4 cameraWorldRow2;
    DirectX::XMFLOAT4 cameraWorldRow3;

    DirectX::XMFLOAT4 cameraPreviousWorldRow0;
    DirectX::XMFLOAT4 cameraPreviousWorldRow1;
    DirectX::XMFLOAT4 cameraPreviousWorldRow2;
    DirectX::XMFLOAT4 cameraPreviousWorldRow3;

    // Live engine fog state, sampled from RE::Sky each frame. These are the
    // already-blended values the engine fog passes consume (not the per-weather
    // form data), so they track weather transitions and interior overrides
    // automatically.
    DirectX::XMFLOAT4 g_FogDistances0;  // x=near, y=far, z=waterNear, w=waterFar
    DirectX::XMFLOAT4 g_FogDistances1;  // x=heightMid, y=heightRange, z=farHeightMid, w=farHeightRange
    DirectX::XMFLOAT4 g_FogParams;      // x=fogHeight, y=fogPower, z=fogClamp, w=fogHighDensityScale
    DirectX::XMFLOAT4 g_FogColor;       // x,y,z=blended fog RGB (0 until per-weather blend lands), w=reserved

    // Dominant stylized world light. Exterior color is Sky::GetSunLightColor
    // (skyColor[4], Sky+0x0D8); interior color resolves cell/template
    // directional light. Direction is best-effort: exterior sun node forward,
    // interior decoded cell/template directional angles.
    float g_SunR;
    float g_SunG;
    float g_SunB;
    float g_SunDirX;

    float g_SunDirY;
    float g_SunDirZ;
    float g_SunValid;
    float g_SunPadding;

    // L1 spherical-harmonics ambient, computed from RE::Sky's 6-axis
    // directional ambient cube (Sky+0x3B8, NiColor[3][2]). The engine's own
    // deferred SH pass (CAEE89E9) consumes a similarly packed cb2[6..8],
    // but its bind timing is unreliable for shaders that run early in the
    // deferred pipeline (e.g. OG) and the engine only emits the SH pass when
    // it deems SH "meaningful," leaving OG-only permutations (capture 121152)
    // with no ambient signal at all. Sourcing the SH ourselves and routing
    // through GFXInjected closes that gap deterministically: any deferred
    // shader can evaluate dot(g_SH_*, float4(N, 1)) per pixel and recover the
    // engine's directional ambient irradiance regardless of which
    // permutation the engine emitted this frame.
    //
    // Packing per channel: .x = X+ - X- band, .y = Y+ - Y-, .z = Z+ - Z-,
    // .w = mean of all 6 directions (DC term). Linear domain — no gamma
    // encoding, so shaders evaluate dot() and use the result directly.
    DirectX::XMFLOAT4 g_SH_R;
    DirectX::XMFLOAT4 g_SH_G;
    DirectX::XMFLOAT4 g_SH_B;

    // BSGraphics camera translation is not part of viewMat/viewProjMat.
    // Reconstructed depth positions are therefore camera-relative, while
    // renderer light geometry is absolute world space. These exact
    // CameraStateData values let traced passes rebase both local lights and
    // previous-frame reprojection without changing the long-standing
    // camera-relative contract of ReconstructWorldPos.
    DirectX::XMFLOAT4 g_CurrentCameraPositionAdjust;
    DirectX::XMFLOAT4 g_PreviousCameraPositionAdjust;

    // xy is the logical projection/render extent. zw is the display extent.
    // They differ under native dynamic resolution and external upscalers.
    DirectX::XMFLOAT4 g_RenderInfo;

    // World-space copy of RE::Sky's directional ambient cube. Keep this at
    // the end of the structured-buffer ABI so older compiled shaders retain
    // the offsets of every pre-existing field. Voxel and screen-space ray
    // passes operate in world space and can evaluate these coefficients
    // directly, without the camera-dependent basis used by the legacy g_SH
    // fields above.
    DirectX::XMFLOAT4 g_WorldSH_R;
    DirectX::XMFLOAT4 g_WorldSH_G;
    DirectX::XMFLOAT4 g_WorldSH_B;
};

struct alignas(16) DrawTagData
{
    float materialTag;
    // 1.0 if the geometry is reachable from the actor's BSFaceGenNiNode subtree
    // after filtering hair headparts. 0.0 otherwise. Lets pixel shaders that share the
    // actor materialTag with equipment distinguish facegen meshes — e.g. for
    // toon face shading or eye-white tinting on custom races.
    float isHead;
    // Bitmask populated from ShaderEngine.ini RACE_GROUP_N entries.
    std::uint32_t raceGroupMask;
    // Bit 0 = actor race was resolved. raceGroupMask==0 with this bit set means
    // "known race, no configured group"; without it means unknown/unresolved.
    std::uint32_t raceFlags;
};

struct RaceGroupFormRef
{
    std::string pluginName;
    std::uint32_t formID = 0;
    std::uint32_t groupMask = 0;
};

// Shader settings value
struct ShaderValue {
    bool global = false; // If true, this value is global and not per-shader
    std::string folderName = ""; // Folder/module this value was loaded from
    std::string shaderDefinitionId = ""; // The ID of the shader definition this value belongs to, empty for global values
    std::string id = ""; // Unique ID for this value
    std::string label = ""; // Label to show in UI
    std::string group = ""; // Optional group name to organize values in the UI
    std::string tooltip = ""; // Optional hover help loaded from Values.ini
    enum class Type { Float, Int, Bool } type = Type::Float; // Type of the value for UI and storage
    // Value Tracking
    struct Data {
        float f;
        int i;
        bool b;
    } current, min, max, step, def;
    uint32_t bufferIndex = 0; // The slot index (0-199 for floats, etc.)
    void ResetToDefault() {
        switch (type) {
            case Type::Float: current.f = def.f; break;
            case Type::Int: current.i = def.i; break;
            case Type::Bool: current.b = def.b; break;
        }
    }
    // Helper to get the byte offset within the SRV buffer based on type and index
};

// Global shader settings holder
class GlobalShaderSettings {
    std::vector<ShaderValue*> globalShaderValues;
    std::vector<ShaderValue*> localShaderValues;
    std::vector<ShaderValue*> boolShaderValues;
    std::vector<ShaderValue*> floatShaderValues;
    std::vector<ShaderValue*> intShaderValues;
public:
    // Add a new shader value to the appropriate vector based on its type.
    // Returns whether the value was inserted anywhere; on false (a value
    // with this identity is already registered - the normal case when
    // Values.ini is RE-parsed by the full reload path) the caller still
    // owns the pointer and must delete it. Existing entries keep their
    // live (user-tuned) current values.
    bool AddShaderValue(ShaderValue* value) {
        if (!value) return false;
        // Check if we have this id already
        auto dedupCheck = [&](const std::vector<ShaderValue*>& vec) {
            return std::any_of(vec.begin(), vec.end(), [&](ShaderValue* s) {
                return s &&
                    s->id == value->id &&
                    s->shaderDefinitionId == value->shaderDefinitionId &&
                    s->folderName == value->folderName &&
                    s->global == value->global;
        });
        };
        bool added = false;
        if (value->global) {
            if (!dedupCheck(globalShaderValues)) {
                globalShaderValues.push_back(value);
                added = true;
            }
        } else {
            if (!dedupCheck(localShaderValues)) {
                localShaderValues.push_back(value);
                added = true;
            }
        }
        if (value->type == ShaderValue::Type::Bool) {
            if (!dedupCheck(boolShaderValues)) {
                value->bufferIndex = static_cast<uint32_t>(boolShaderValues.size());
                boolShaderValues.push_back(value);
                added = true;
            }
        } else if (value->type == ShaderValue::Type::Float) {
            if (!dedupCheck(floatShaderValues)) {
                value->bufferIndex = static_cast<uint32_t>(floatShaderValues.size());
                floatShaderValues.push_back(value);
                added = true;
            }
        } else if (value->type == ShaderValue::Type::Int) {
            if (!dedupCheck(intShaderValues)) {
                value->bufferIndex = static_cast<uint32_t>(intShaderValues.size());
                intShaderValues.push_back(value);
                added = true;
            }
        }
        return added;
    }
    // Save the current shader settings values to a file (e.g. JSON or INI)
    bool SaveSettings(std::string* errorMessage = nullptr) {
        std::filesystem::path settingsPath = g_pluginPath / "ShaderEngineShaderSettings.ini";
        std::ofstream file(settingsPath, std::ios::out);
        if (!file.is_open()) {
            if (errorMessage) {
                *errorMessage = "Failed to open " + settingsPath.string();
            }
            return false;
        }

        // Save global shader values
        file << "; Global Shader Values\n";
        for (const auto* sValue : globalShaderValues) {
            if (!sValue) continue;
            if (sValue) {
                file << "[global]\n";
                file << "folderName=" << sValue->folderName << "\n";
                file << "id=" << sValue->id << "\n";
                file << "label=" << sValue->label << "\n";
                file << "group=" << sValue->group << "\n";
                file << "type=";
                switch (sValue->type) {
                    case ShaderValue::Type::Float: file << "float"; break;
                    case ShaderValue::Type::Int: file << "int"; break;
                    case ShaderValue::Type::Bool: file << "bool"; break;
                };
                file << "\n";
                file << "value=";
                switch (sValue->type) {
                    case ShaderValue::Type::Float: file << sValue->current.f; break;
                    case ShaderValue::Type::Int: file << sValue->current.i; break;
                    case ShaderValue::Type::Bool: file << (sValue->current.b ? "true" : "false"); break;
                };
                file << "\n";
                file << "[/global]\n\n";
            }
        }
        // Save local shader values
        file << "; Local Shader Values\n";
        for (const auto* sValue : localShaderValues) {
            if (!sValue) continue;
            if (sValue) {
                file << "[local]\n";
                file << "folderName=" << sValue->folderName << "\n";
                file << "shaderDefinitionId=" << sValue->shaderDefinitionId << "\n";
                file << "id=" << sValue->id << "\n";
                file << "label=" << sValue->label << "\n";
                file << "group=" << sValue->group << "\n";
                file << "type=";
                switch (sValue->type) {
                    case ShaderValue::Type::Float: file << "float"; break;
                    case ShaderValue::Type::Int: file << "int"; break;
                    case ShaderValue::Type::Bool: file << "bool"; break;
                };
                file << "\n";
                file << "value=";
                switch (sValue->type) {
                    case ShaderValue::Type::Float: file << sValue->current.f; break;
                    case ShaderValue::Type::Int: file << sValue->current.i; break;
                    case ShaderValue::Type::Bool: file << (sValue->current.b ? "true" : "false"); break;
                };
                file << "\n";
                file << "[/local]\n\n";
            }
        }
        file.close();
        if (!file) {
            if (errorMessage) {
                *errorMessage = "Failed to write " + settingsPath.string();
            }
            return false;
        }
        return true;
    }
    // Load shader settings values from a file and update the current values
    void LoadSettings() {
        std::filesystem::path settingsPath = g_pluginPath / "ShaderEngineShaderSettings.ini";
        std::ifstream file(settingsPath, std::ios::in);
        // Read the entire file into a string for easier parsing
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        // Helper function to parse the file for global and local shader values
        auto parseValue = [&](ShaderValue* sValue) {
            if (!sValue) return;
            const std::string sectionStart = sValue->global ? "[global]" : "[local]";
            const std::string sectionEnd = sValue->global ? "[/global]" : "[/local]";
            std::string val;
            size_t searchPos = 0;
            while (true) {
                size_t blockStart = text.find(sectionStart, searchPos);
                if (blockStart == std::string::npos) return;
                size_t blockEnd = text.find(sectionEnd, blockStart);
                if (blockEnd == std::string::npos) return;
                std::string block = text.substr(blockStart, blockEnd - blockStart);
                const bool idMatches = block.find("id=" + sValue->id + "\n") != std::string::npos ||
                    block.find("id=" + sValue->id + "\r\n") != std::string::npos;
                const bool folderMatches = sValue->folderName.empty() ||
                    block.find("folderName=" + sValue->folderName + "\n") != std::string::npos ||
                    block.find("folderName=" + sValue->folderName + "\r\n") != std::string::npos;
                const bool definitionMatches = sValue->global || sValue->shaderDefinitionId.empty() ||
                    block.find("shaderDefinitionId=" + sValue->shaderDefinitionId + "\n") != std::string::npos ||
                    block.find("shaderDefinitionId=" + sValue->shaderDefinitionId + "\r\n") != std::string::npos;
                if (idMatches && folderMatches && definitionMatches) {
                    size_t valpos = block.find("value=");
                    if (valpos == std::string::npos) return;
                    valpos += 6;
                    size_t end = block.find('\n', valpos);
                    val = block.substr(valpos, end - valpos);
                    if (!val.empty() && val.back() == '\r') {
                        val.pop_back();
                    }
                    break;
                }
                searchPos = blockEnd + sectionEnd.size();
            }
            // convert based on type
            try {
                switch (sValue->type) {
                case ShaderValue::Type::Bool:
                    sValue->current.b = (ToLower(val) == "true" || val == "1");
                    break;
                case ShaderValue::Type::Float:
                    sValue->current.f = std::stof(val);
                    break;
                case ShaderValue::Type::Int:
                    sValue->current.i = std::stoi(val);
                    break;
                }
            } catch (...) { /* ignore bad lines */ }
        };
        // Parse global shader values
        for (auto* sValue : globalShaderValues) {
            parseValue(sValue);
        }
        // Parse local shader values
        for (auto* sValue : localShaderValues) {
            parseValue(sValue);
        }
    }
    std::vector<ShaderValue*>& GetGlobalShaderValues() {
        return globalShaderValues;
    }
    std::vector<ShaderValue*>& GetLocalShaderValues() {
        return localShaderValues;
    }
    std::vector<ShaderValue*>& GetFloatShaderValues() {
        return floatShaderValues;
    }
    std::vector<ShaderValue*>& GetIntShaderValues() {
        return intShaderValues;
    }
    std::vector<ShaderValue*>& GetBoolShaderValues() {
        return boolShaderValues;
    }
};

// ENUM for shader type
enum class ShaderType {
    Vertex,
    Pixel
};

// Shader size requirement operator for matching definitions
enum class SizeOp { Equal, Greater, Less };
struct SizeRequirement {
    SizeOp op;
    std::size_t value;
};
struct InputCountRequirement {
    SizeOp op;
    int value;
};
struct OutputCountRequirement {
    SizeOp op;
    int value;
};

struct ReplacementTextureBinding {
    int slot = -1;
    std::filesystem::path file;
    REX::W32::ID3D11ShaderResourceView* srv = nullptr;
    bool loadTried = false;
    bool loadFailed = false;

    void Release()
    {
        if (srv) {
            srv->Release();
            srv = nullptr;
        }
        loadTried = false;
        loadFailed = false;
    }
};

enum class ReplacementSRVSourceKind : uint8_t {
    None,
    Depth,
    SceneHDR,
    GBufferRT,
    GBufferNormal,
    GBufferAlbedo,
    GBufferMaterial,
    MotionVectors,
    CustomResource
};

struct ReplacementSRVBinding {
    int slot = -1;
    ReplacementSRVSourceKind kind = ReplacementSRVSourceKind::None;
    std::string resourceName;
    int gbufferIndex = -1;
    bool warnedMissing = false;
};

// Shader definitions from INI file
struct ShaderDefinition {
    ShaderDefinition() = default;
    ShaderDefinition(const ShaderDefinition&) = delete;
    ShaderDefinition& operator=(const ShaderDefinition&) = delete;
    ShaderDefinition(ShaderDefinition&& other) noexcept
        : id(std::move(other.id))
        , active(other.active)
        , priority(other.priority)
        , type(other.type)
        , shaderUID(std::move(other.shaderUID))
        , hash(std::move(other.hash))
        , asmHash(std::move(other.asmHash))
        , fxpPixelShaderID(std::move(other.fxpPixelShaderID))
        , fxpShaderType(std::move(other.fxpShaderType))
        , sizeRequirements(std::move(other.sizeRequirements))
        , bufferSizes(std::move(other.bufferSizes))
        , textureSlots(std::move(other.textureSlots))
        , textureDimensions(std::move(other.textureDimensions))
        , textureSlotMask(other.textureSlotMask)
        , textureDimensionMask(other.textureDimensionMask)
        , inputTextureCountRequirements(std::move(other.inputTextureCountRequirements))
        , inputCountRequirements(std::move(other.inputCountRequirements))
        , inputMask(other.inputMask)
        , outputCountRequirements(std::move(other.outputCountRequirements))
        , outputMask(other.outputMask)
        , shaderFile(std::move(other.shaderFile))
        , shadowUpgrade(other.shadowUpgrade)
        , buggy(other.buggy)
        , compiledShader(other.compiledShader)
        , loadedPixelShader(other.loadedPixelShader)
        , loadedVertexShader(other.loadedVertexShader)
        , replacementTextures(std::move(other.replacementTextures))
        , replacementSRVs(std::move(other.replacementSRVs))
        , usesGFXInjected(other.usesGFXInjected)
        , usesGFXDrawTag(other.usesGFXDrawTag)
        , usesGFXModularFloats(other.usesGFXModularFloats)
        , usesGFXModularInts(other.usesGFXModularInts)
        , usesGFXModularBools(other.usesGFXModularBools)
        , compileMutex(std::move(other.compileMutex))
        , hlslFileWatcher(std::move(other.hlslFileWatcher))
        , appliedReloadGeneration(other.appliedReloadGeneration)
        , log(other.log)
        , dump(other.dump)
        , lightCullRadiusScaleValue(std::move(other.lightCullRadiusScaleValue))
        , lightCullRadiusScaleResolved(other.lightCullRadiusScaleResolved)
        , lightCullRadiusScaleGpuRef(other.lightCullRadiusScaleGpuRef)
        , lightCullRadiusScaleResolveLogged(other.lightCullRadiusScaleResolveLogged)
        , shaderValues(std::move(other.shaderValues))
    {
        other.compiledShader = nullptr;
        other.loadedPixelShader = nullptr;
        other.loadedVertexShader = nullptr;
        for (auto& binding : other.replacementTextures) {
            binding.srv = nullptr;
        }
    }
    ShaderDefinition& operator=(ShaderDefinition&& other) = delete;

    std::string id;
    bool active = false;
    int priority = 0;
    // Matching criteria
    ShaderType type = ShaderType::Pixel;
    std::vector<std::string> shaderUID = {};
    std::vector<std::uint32_t> hash = {};
    std::vector<std::uint32_t> asmHash = {};
    // Exact native FXP identity. Unlike bytecode hashes, these are the keys
    // BGS itself uses to select a permutation from a BSShader family.
    std::vector<std::uint32_t> fxpPixelShaderID = {};
    std::vector<std::uint32_t> fxpShaderType = {};
    std::vector<SizeRequirement> sizeRequirements;
    std::vector<std::pair<int, int>> bufferSizes;
    std::vector<int> textureSlots;
    std::vector<std::pair<int, int>> textureDimensions;
    std::uint32_t textureSlotMask = 0;
    std::uint32_t textureDimensionMask = 0;
    std::vector<InputCountRequirement> inputTextureCountRequirements = {};
    std::vector<InputCountRequirement> inputCountRequirements = {};
    std::uint32_t inputMask = 0;
    std::vector<OutputCountRequirement> outputCountRequirements = {};
    std::uint32_t outputMask = 0;
    // Replacement shader info
    std::filesystem::path shaderFile = "";
    // Groups the shader under the shadow-upgrade runtime switch. Once enabled,
    // ShaderDB identity and the normal ShaderEngine replacement path remain
    // authoritative; renderer-phase hooks do not second-guess permutations.
    bool shadowUpgrade = false;
    // Compile state
    bool buggy = false;
    ID3DBlob* compiledShader = nullptr;
    REX::W32::ID3D11PixelShader* loadedPixelShader = nullptr;
    REX::W32::ID3D11VertexShader* loadedVertexShader = nullptr;
    std::vector<ReplacementTextureBinding> replacementTextures;
    std::vector<ReplacementSRVBinding> replacementSRVs;
    bool usesGFXInjected = false;
    bool usesGFXDrawTag = false;
    bool usesGFXModularFloats = false;
    bool usesGFXModularInts = false;
    bool usesGFXModularBools = false;
    // Per-definition compile mutex. Held by CompileShader_Internal so the
    // background precompile worker and the render thread can't both try to
    // compile the same def at once (which would leak one of the two
    // CreatePixelShader results). After whichever side wins, the loser
    // takes the lock, sees loadedPixelShader/loadedVertexShader populated,
    // and short-circuits. unique_ptr because std::mutex is non-movable but
    // ShaderDefinition is moved by value at parse time.
    std::unique_ptr<std::mutex> compileMutex = std::make_unique<std::mutex>();
    // File watcher for this shader definition Shader.ini
    std::unique_ptr<HlslFileWatcher> hlslFileWatcher;
    // Last value of g_manualShaderReloadGeneration this definition has acted
    // on. Only ever read/written by the render thread inside
    // MaybeApplyHlslHotReload_Internal, so it needs no synchronization of its
    // own. A definition created after a manual reload starts at 0 and takes
    // one no-op drop on its next bind (it has nothing compiled yet).
    std::uint64_t appliedReloadGeneration = 0;
    // Logging and dumping options
    bool log = false;
    bool dump = false;
    // Optional: name of a Float ShaderValue (declared in some folder's
    // Values.ini, [global] or [local]) whose current value gets multiplied
    // into BSLight::geometry[+0x138] (the bound radius) every time the engine
    // runs BSLight::TestFrustumCull — but ONLY while this rule has a compiled
    // replacement pixel shader AND active=true. Lets a PS replacement that
    // boosts a light's apparent intensity expand the engine's cull bound so
    // distant pixels still receive the boosted contribution. Empty = no
    // scaling. Lookup is lazy on first cull (Values.ini load order independent).
    // See src/LightCullPolicy.{h,cpp}.
    std::string  lightCullRadiusScaleValue;
    // Resolved cache. Exactly one of {Resolved, GpuRef} is non-null after a
    // successful lookup. GpuRef points into a GpuScalar probe's last-readback
    // float and is updated by GpuScalar::OnFramePresent each frame.
    ShaderValue* lightCullRadiusScaleResolved = nullptr;
    const float* lightCullRadiusScaleGpuRef   = nullptr;
    bool         lightCullRadiusScaleResolveLogged = false;    // one-shot warn
    // Collection of shader values defined for this shader, keyed by the byte offset in the SRV buffer
    std::vector<ShaderValue> shaderValues;

    ~ShaderDefinition()
    {
        if (loadedPixelShader) {
            loadedPixelShader->Release();
            loadedPixelShader = nullptr;
        }
        if (loadedVertexShader) {
            loadedVertexShader->Release();
            loadedVertexShader = nullptr;
        }
        if (compiledShader) {
            compiledShader->Release();
            compiledShader = nullptr;
        }
        for (auto& binding : replacementTextures) {
            binding.Release();
        }
    }
};

// Shader DB Entry primary key is the original shader pointer
struct ShaderDBEntry {
    // Constructors
    // Delete copy operations (atomics can't be copied)
    ShaderDBEntry(const ShaderDBEntry&) = delete;
    ShaderDBEntry& operator=(const ShaderDBEntry&) = delete;
    // Default constructor
    ShaderDBEntry() = default;
    // Move constructor
    ShaderDBEntry(ShaderDBEntry&& other) noexcept
        : originalShader(other.originalShader)
        , type(other.type)
        , shaderUID(other.shaderUID)
        , hash(other.hash)
        , asmHash(other.asmHash)
        , fxpPixelShaderID(other.fxpPixelShaderID)
        , hasFxpPixelShaderID(other.hasFxpPixelShaderID)
        , fxpShaderType(other.fxpShaderType)
        , hasFxpShaderType(other.hasFxpShaderType)
        , size(other.size)
        , textureSlots(std::move(other.textureSlots))
        , textureDimensions(std::move(other.textureDimensions))
        , textureSlotMask(other.textureSlotMask)
        , textureDimensionMask(other.textureDimensionMask)
        , inputTextureCount(other.inputTextureCount)
        , inputCount(other.inputCount)
        , inputMask(other.inputMask)
        , outputCount(other.outputCount)
        , outputMask(other.outputMask)
        , matchedDefinition(other.matchedDefinition)
        , bytecode(std::move(other.bytecode))
    {
        std::memcpy(expectedCBSizes, other.expectedCBSizes, sizeof(expectedCBSizes));
        valid.store(other.valid.load(std::memory_order_relaxed));
        matched.store(other.matched.load(std::memory_order_relaxed));
        dumped.store(other.dumped.load(std::memory_order_relaxed));
        recentlyUsed.store(other.recentlyUsed.load(std::memory_order_relaxed));
        replacementPixelShader.store(other.replacementPixelShader.load(std::memory_order_relaxed));
        replacementVertexShader.store(other.replacementVertexShader.load(std::memory_order_relaxed));
    }
    // Move assignment operator
    ShaderDBEntry& operator=(ShaderDBEntry&& other) noexcept {
        if (this != &other) {
            originalShader = other.originalShader;
            other.originalShader = nullptr; // Prevent dangling pointer
            type = other.type;
            shaderUID = std::move(other.shaderUID);
            hash = other.hash;
            asmHash = other.asmHash;
            fxpPixelShaderID = other.fxpPixelShaderID;
            hasFxpPixelShaderID = other.hasFxpPixelShaderID;
            fxpShaderType = other.fxpShaderType;
            hasFxpShaderType = other.hasFxpShaderType;
            size = other.size;
            textureSlots = std::move(other.textureSlots);
            textureDimensions = std::move(other.textureDimensions);
            textureSlotMask = other.textureSlotMask;
            textureDimensionMask = other.textureDimensionMask;
            inputTextureCount = other.inputTextureCount;
            inputCount = other.inputCount;
            inputMask = other.inputMask;
            outputCount = other.outputCount;
            outputMask = other.outputMask;
            matchedDefinition = other.matchedDefinition;
            bytecode = std::move(other.bytecode);
            std::memcpy(expectedCBSizes, other.expectedCBSizes, sizeof(expectedCBSizes));
            valid.store(other.valid.load(std::memory_order_relaxed));
            matched.store(other.matched.load(std::memory_order_relaxed));
            dumped.store(other.dumped.load(std::memory_order_relaxed));
            recentlyUsed.store(other.recentlyUsed.load(std::memory_order_relaxed));
            replacementPixelShader.store(other.replacementPixelShader.load(std::memory_order_relaxed));
            replacementVertexShader.store(other.replacementVertexShader.load(std::memory_order_relaxed));
        }
        return *this;
    }
    // For identification and matching
    // Will be initialized in the CreatePixelShader hook once and very early in the game
    void* originalShader = nullptr;
    // Matching criteria
    ShaderType type = ShaderType::Pixel;
    std::string shaderUID = "";
    std::uint32_t hash = 0;
    std::uint32_t asmHash = 0;
    // Native Shaders011.fxp lookup key read from BSGraphics::PixelShader+0.
    // Zero for non-pixel shaders and pixel shaders not loaded by BGS FXP.
    std::uint32_t fxpPixelShaderID = 0;
    bool hasFxpPixelShaderID = false;
    // Owning BSShader table. FXP permutation keys are only unique inside this
    // family (4=DFLight, 6=DFComposite on OG).
    std::uint32_t fxpShaderType = 0;
    bool hasFxpShaderType = false;
    std::size_t size = 0;
    std::uint32_t expectedCBSizes[14] = {0};
    std::vector<int> textureSlots = {};
    std::vector<std::pair<int, int>> textureDimensions = {};
    std::uint32_t textureSlotMask = 0;
    std::uint32_t textureDimensionMask = 0;
    int inputTextureCount = 0;
    int inputCount = 0;
    std::uint32_t inputMask = 0;
    int outputCount = 0;
    std::uint32_t outputMask = 0;
    // Shader states
    std::atomic<bool> valid{false}; // Whether this entry is valid (initialized)
    std::atomic<bool> matched{false}; // Whether we found a match for this shader
    std::atomic<bool> dumped{false}; // Whether we've already dumped this shader to disk for analysis
    std::atomic<bool> recentlyUsed{false}; // Whether this shader was used in the most recent frame
    // The matching definition from the INI file
    // We need this to compile the shader when it is needed during rendering
    ShaderDefinition* matchedDefinition = nullptr;
    // Compiled replacement shader
    std::atomic<REX::W32::ID3D11PixelShader*> replacementPixelShader{nullptr};
    std::atomic<REX::W32::ID3D11VertexShader*> replacementVertexShader{nullptr};
    // Info
    std::vector<uint8_t> bytecode = {}; // Raw bytecode for hashing and analysis
    // Helper functions for thread-safe access
    void SetValid(bool value) { valid.store(value, std::memory_order_release); }
    bool IsValid() const { return valid.load(std::memory_order_acquire); }
    void SetMatched(bool value) { matched.store(value, std::memory_order_release); }
    bool IsMatched() const { return matched.load(std::memory_order_acquire); }
    void SetDumped(bool value) { dumped.store(value, std::memory_order_release); }
    bool IsDumped() const { return dumped.load(std::memory_order_acquire); }
    ShaderDefinition* GetMatchedDefinition() const { return matchedDefinition; }
    void SetRecentlyUsed(bool value) { recentlyUsed.store(value, std::memory_order_release); }
    bool IsRecentlyUsed() const { return recentlyUsed.load(std::memory_order_acquire); }
    void SetReplacementPixelShader(REX::W32::ID3D11PixelShader* shader) {
        replacementPixelShader.store(shader, std::memory_order_release);
    }
    REX::W32::ID3D11PixelShader* GetReplacementPixelShader() const {
        return replacementPixelShader.load(std::memory_order_acquire);
    }
    void SetReplacementVertexShader(REX::W32::ID3D11VertexShader* shader) {
        replacementVertexShader.store(shader, std::memory_order_release);
    }
    REX::W32::ID3D11VertexShader* GetReplacementVertexShader() const {
        return replacementVertexShader.load(std::memory_order_acquire);
    }
};

// Database for storing shader definitions loaded from the INI file
struct ShaderDefDB {
    std::vector<ShaderDefinition*> definitions;
    mutable std::shared_mutex mutex;
    // Add a new shader definition to the database
    void AddDefinition(ShaderDefinition* def) {
        std::unique_lock lock(mutex);
        definitions.push_back(def);
    }
    // Sort definitions by priority (lower number = higher priority)
    void SortByPriority() {
        std::unique_lock lock(mutex);
        std::sort(definitions.begin(), definitions.end(), [](const ShaderDefinition* a, const ShaderDefinition* b) {
            return a->priority < b->priority;
        });
    }
    // Clear the shader definition database
    void Clear() {
        std::unique_lock lock(mutex);
        for (auto def : definitions) {
            delete def;
        }
        definitions.clear();
    }
};

// Shader database for storing parsed shader info for all shaders we encounter
struct ShaderDB {
    // Key = original shader pointer
    std::unordered_map<void*, ShaderDBEntry> entries;
    mutable std::shared_mutex mutex;
    // Functions for managing the shader database
    void AddShaderEntry(ShaderDBEntry&& entry) {
        if (!entry.IsValid()) return; // Invalid entry, skip
        std::unique_lock lock(mutex); // Exclusive for writes
        entries[entry.originalShader] = std::move(entry);
    }
    bool HasEntry(REX::W32::ID3D11PixelShader* shader) {
        std::shared_lock lock(mutex);
        return entries.find(shader) != entries.end();
    }
    bool HasEntry(REX::W32::ID3D11VertexShader* shader) {
        std::shared_lock lock(mutex);
        return entries.find(shader) != entries.end();
    }
    bool IsEntryMatched(REX::W32::ID3D11PixelShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.matched.load() : false;
    }
    bool IsEntryMatched(REX::W32::ID3D11VertexShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.matched.load() : false;
    }
    void SetEntryMatched(REX::W32::ID3D11PixelShader* shader, bool matched) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        if (it != entries.end()) {
            it->second.SetMatched(matched);
        }
    }
    void SetEntryMatched(REX::W32::ID3D11VertexShader* shader, bool matched) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        if (it != entries.end()) {
            it->second.SetMatched(matched);
        }
    }
    bool IsEntryDumped(REX::W32::ID3D11PixelShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.dumped.load() : false;
    }
    bool IsEntryDumped(REX::W32::ID3D11VertexShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.dumped.load() : false;
    }
    void SetEntryDumped(REX::W32::ID3D11PixelShader* shader, bool dumped) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        if (it != entries.end()) {
            it->second.SetDumped(dumped);
        }
    }
    void SetEntryDumped(REX::W32::ID3D11VertexShader* shader, bool dumped) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        if (it != entries.end()) {
            it->second.SetDumped(dumped);
        }
    }
    bool IsEntryRecentlyUsed(REX::W32::ID3D11PixelShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.IsRecentlyUsed() : false;
    }
    bool IsEntryRecentlyUsed(REX::W32::ID3D11VertexShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.IsRecentlyUsed() : false;
    }
    void SetEntryRecentlyUsed(REX::W32::ID3D11PixelShader* shader, bool recentlyUsed) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        if (it != entries.end()) {
            it->second.SetRecentlyUsed(recentlyUsed);
        }
    }
    void SetEntryRecentlyUsed(REX::W32::ID3D11VertexShader* shader, bool recentlyUsed) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        if (it != entries.end()) {
            it->second.SetRecentlyUsed(recentlyUsed);
        }
    }
    ShaderDefinition* GetMatchedDefinition(REX::W32::ID3D11PixelShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.GetMatchedDefinition() : nullptr;
    }
    ShaderDefinition* GetMatchedDefinition(REX::W32::ID3D11VertexShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.GetMatchedDefinition() : nullptr;
    }
    REX::W32::ID3D11PixelShader* GetReplacementShader(REX::W32::ID3D11PixelShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.GetReplacementPixelShader() : nullptr;
    }
    REX::W32::ID3D11VertexShader* GetReplacementShader(REX::W32::ID3D11VertexShader* shader) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        return (it != entries.end()) ? it->second.GetReplacementVertexShader() : nullptr;
    }
    void SetReplacementShader(REX::W32::ID3D11PixelShader* shader, REX::W32::ID3D11PixelShader* replacement) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        if (it != entries.end()) {
            it->second.SetReplacementPixelShader(replacement);
        }
    }
    void SetReplacementShader(REX::W32::ID3D11VertexShader* shader, REX::W32::ID3D11VertexShader* replacement) {
        std::shared_lock lock(mutex);
        auto it = entries.find(shader);
        if (it != entries.end()) {
            it->second.SetReplacementVertexShader(replacement);
        }
    }
    // Publish a successfully created replacement object to every vanilla
    // shader entry already matched to this definition. The definition owns
    // the D3D object; entries only cache its pointer for render-thread lookup.
    //
    // Background precompilation creates def->loaded*Shader before a vanilla
    // permutation necessarily executes. Without this publication step the
    // monitor reports INVALID and the entry remains empty until PSSetShader /
    // VSSetShader happens to bind that exact permutation.
    void PublishReplacementForDefinition(ShaderDefinition* def) {
        if (!def) return;
        std::shared_lock lock(mutex);
        for (auto& [shader, entry] : entries) {
            if (entry.matchedDefinition != def) continue;
            if (def->type == ShaderType::Pixel) {
                entry.SetReplacementPixelShader(def->loadedPixelShader);
            } else {
                entry.SetReplacementVertexShader(def->loadedVertexShader);
            }
        }
    }
    void ClearReplacementsForDefinition(ShaderDefinition* def) {
        // Mutates entries — must be exclusive. Was previously a shared_lock
        // which raced when multiple watcher threads / hot-reload paths fired
        // at once.
        std::unique_lock lock(mutex);
        for (auto& [shader, entry] : entries) {
            if (entry.matchedDefinition == def) {
                entry.SetReplacementPixelShader(nullptr);
                entry.SetReplacementVertexShader(nullptr);
            }
        }
    }
    void Clear() {
        std::unique_lock lock(mutex); // Exclusive for writes
        entries.clear();
    }
    void UnmatchAll() {
        std::unique_lock lock(mutex);
        for (auto& [shader, entry] : entries) {
            entry.SetMatched(false);
            entry.matchedDefinition = nullptr;
            entry.SetReplacementPixelShader(nullptr);
            entry.SetReplacementVertexShader(nullptr);
        }
    }
};

// --- Functions ---

struct D3D11PSSetShaderResult {
    REX::W32::ID3D11PixelShader* shader = nullptr;
    ShaderDefinition* activeReplacementDef = nullptr;
    bool usingReplacementPixelShader = false;
    bool customPassFired = false;
};

struct BSRenderPassLayout
{
    BSRenderPassLayout* next;                  // 00
    RE::BSShader* shader;                      // 08
    RE::BSShaderProperty* shaderProperty;      // 10
    RE::BSGeometry* geometry;                  // 18
    std::byte pad20[0x28 - 0x20];              // 20
    void* commandBuffer;                       // 28
    std::byte pad30[0x38 - 0x30];              // 30
    BSRenderPassLayout* listNext;              // 38
    BSRenderPassLayout* passGroupNext;         // 40
    std::uint32_t techniqueID;                 // 48
};
static_assert(offsetof(BSRenderPassLayout, shaderProperty) == 0x10);
static_assert(offsetof(BSRenderPassLayout, geometry) == 0x18);
static_assert(offsetof(BSRenderPassLayout, commandBuffer) == 0x28);
static_assert(offsetof(BSRenderPassLayout, listNext) == 0x38);
static_assert(offsetof(BSRenderPassLayout, passGroupNext) == 0x40);
static_assert(offsetof(BSRenderPassLayout, techniqueID) == 0x48);

ShaderDBEntry AnalyzeShader_Internal(
    REX::W32::ID3D11PixelShader* pixelShader,
    REX::W32::ID3D11VertexShader* vertexShader,
    std::vector<uint8_t> bytecode,
    SIZE_T bytecodeLength,
    std::uint32_t fxpPixelShaderID = 0,
    bool hasFxpPixelShaderID = false,
    std::uint32_t fxpShaderType = 0,
    bool hasFxpShaderType = false);
bool CompileShader_Internal(ShaderDefinition* def);
bool DoesEntryMatchDefinition_Internal(ShaderDBEntry const& entry, ShaderDefinition* def);
void DumpOriginalShader_Internal(ShaderDBEntry const& entry, ShaderDefinition* def);
void ClearActorDrawTaggedGeometry_Internal();
void ReleaseDrawTagBuffers_Internal();
bool InstallGFXHooks_Internal();
bool InstallShaderCreationHooks_Internal();
bool InstallDrawTaggingHooks_Internal();
bool ReflectShader_Internal(ShaderDBEntry& entry);
void ResetPlayerRadDamageTracking();
void ReloadAllShaderDefinitions_Internal();
void RematchAllShaders_Internal();
// If the HLSL watcher for `def` flagged a disk change, drop the cached
// compiled shader / replacement pointers so the next compile path picks up
// the fresh source. Must be called from the render thread (does D3D11
// Release on the immediate-context-owning thread); a no-op otherwise.
void MaybeApplyHlslHotReload_Internal(ShaderDefinition* def);
void ShaderDumpWorker();
void ShutdownShaderDumping_Internal();
void UILockShaderList_Internal();
void UIUnlockShaderList_Internal();
std::uint64_t GetD3DDrawCallsLastFrame_Internal();
void ArmCustomPassDrawBatch(REX::W32::ID3D11PixelShader* originalPS);
bool FireArmedCustomPassDrawBatch(REX::W32::ID3D11DeviceContext* context, const char* source);
bool IsPrecombineShadowGeometry_Internal(RE::BSGeometry* geometry);
bool IsActorDrawTaggedGeometry_Internal(RE::BSGeometry* geometry);
bool IsRenderBatchesActive_Internal();
BSRenderPassLayout* GetRenderBatchNodeHead_Internal(void* batchRenderer, int group, unsigned int subIdx);
