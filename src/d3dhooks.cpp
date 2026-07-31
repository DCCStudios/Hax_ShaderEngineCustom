#include <Global.h>
#include <PCH.h>
#include <CustomPass.h>
#include <Plugin.h>
#include "d3dhooks.h"
#include <GpuScalar.h>
#include <hooks.h>
#include <ImguiMenu.h>
#include <LightTracker.h>
#include <PhaseTelemetry.h>
#include <RenderTargets.h>
#include <ShaderPipeline.h>
#include <ShaderResources.h>
#include <ShadowMapDeferredLighting.h>
#include <ShadowTelemetry.h>
#include <TiledDeferredLighting.h>

extern HWND g_outputWindow;
extern std::atomic<REX::W32::ID3D11PixelShader*> g_currentOriginalPixelShader;

namespace D3D11Hooks
{
    D3D11CreateDeviceAndSwapChain_t OriginalD3D11CreateDeviceAndSwapChain = nullptr;
    ClipCursor_t OriginalClipCursor = nullptr;
    Present_t OriginalPresent = nullptr;
    PSSetShaderResources_t OriginalPSSetShaderResources = nullptr;
    OMSetRenderTargets_t OriginalOMSetRenderTargets = nullptr;
    ClearDepthStencilView_t OriginalClearDepthStencilView = nullptr;
    DrawIndexed_t OriginalDrawIndexed = nullptr;
    Draw_t OriginalDraw = nullptr;
    DrawIndexedInstanced_t OriginalDrawIndexedInstanced = nullptr;
    DrawInstanced_t OriginalDrawInstanced = nullptr;
    Map_t OriginalMap = nullptr;
    Unmap_t OriginalUnmap = nullptr;
    SetConstantBuffers_t OriginalVSSetConstantBuffers = nullptr;
    SetConstantBuffers_t OriginalPSSetConstantBuffers = nullptr;
    SetSamplers_t OriginalPSSetSamplers = nullptr;
    IASetInputLayout_t OriginalIASetInputLayout = nullptr;
    IASetVertexBuffers_t OriginalIASetVertexBuffers = nullptr;
    IASetIndexBuffer_t OriginalIASetIndexBuffer = nullptr;
    OMSetBlendState_t OriginalOMSetBlendState = nullptr;
    OMSetDepthStencilState_t OriginalOMSetDepthStencilState = nullptr;
    SetState1_t OriginalRSSetState = nullptr;
    PSSetShader_t OriginalPSSetShader = nullptr;
    VSSetShader_t OriginalVSSetShader = nullptr;
    CreatePixelShader_t OriginalCreatePixelShader = nullptr;
    CreateVertexShader_t OriginalCreateVertexShader = nullptr;
}

RE::PlayerCharacter* g_player = nullptr;
RE::ActorValue* g_actorValueInfo = nullptr;
RE::Sky* g_sky = nullptr;

static std::atomic<std::uint64_t> g_d3dDrawCallsThisFrame{ 0 };
static std::atomic<std::uint64_t> g_d3dDrawCallsLastFrame{ 0 };
static std::atomic_bool g_customBufferRefreshedBeforePresent{ false };

namespace
{
    constexpr UINT kReplaySRVCacheSlots = 128;
    constexpr UINT kReplayVertexBufferSlots = 32;
    constexpr std::chrono::seconds kReplayMapTelemetryInterval{ 2 };

    struct DrawIndexedPayload
    {
        UINT indexCount;
        UINT startIndexLocation;
        INT baseVertexLocation;
    };

    struct DrawPayload
    {
        UINT vertexCount;
        UINT startVertexLocation;
    };

    struct DrawIndexedInstancedPayload
    {
        UINT indexCountPerInstance;
        UINT instanceCount;
        UINT startIndexLocation;
        INT baseVertexLocation;
        UINT startInstanceLocation;
    };

    struct DrawInstancedPayload
    {
        UINT vertexCountPerInstance;
        UINT instanceCount;
        UINT startVertexLocation;
        UINT startInstanceLocation;
    };

    void ReplayDrawIndexed(
        REX::W32::ID3D11DeviceContext* context,
        const void* rawPayload) noexcept
    {
        const auto& args =
            *static_cast<const DrawIndexedPayload*>(rawPayload);
        D3D11Hooks::OriginalDrawIndexed(
            context,
            args.indexCount,
            args.startIndexLocation,
            args.baseVertexLocation);
    }

    void ReplayDraw(
        REX::W32::ID3D11DeviceContext* context,
        const void* rawPayload) noexcept
    {
        const auto& args =
            *static_cast<const DrawPayload*>(rawPayload);
        D3D11Hooks::OriginalDraw(
            context,
            args.vertexCount,
            args.startVertexLocation);
    }

    void ReplayDrawIndexedInstanced(
        REX::W32::ID3D11DeviceContext* context,
        const void* rawPayload) noexcept
    {
        const auto& args =
            *static_cast<const DrawIndexedInstancedPayload*>(rawPayload);
        D3D11Hooks::OriginalDrawIndexedInstanced(
            context,
            args.indexCountPerInstance,
            args.instanceCount,
            args.startIndexLocation,
            args.baseVertexLocation,
            args.startInstanceLocation);
    }

    void ReplayDrawInstanced(
        REX::W32::ID3D11DeviceContext* context,
        const void* rawPayload) noexcept
    {
        const auto& args =
            *static_cast<const DrawInstancedPayload*>(rawPayload);
        D3D11Hooks::OriginalDrawInstanced(
            context,
            args.vertexCountPerInstance,
            args.instanceCount,
            args.startVertexLocation,
            args.startInstanceLocation);
    }

    struct ReplaySRVCallCache
    {
        std::array<bool, kReplaySRVCacheSlots> valid{};
        std::array<REX::W32::ID3D11ShaderResourceView*, kReplaySRVCacheSlots> views{};
    };

    struct ReplayVertexBufferState
    {
        REX::W32::ID3D11Buffer* buffer = nullptr;
        UINT stride = 0;
        UINT offset = 0;
    };

    struct ReplayIACache
    {
        bool inputLayoutValid = false;
        REX::W32::ID3D11InputLayout* inputLayout = nullptr;

        bool indexBufferValid = false;
        REX::W32::ID3D11Buffer* indexBuffer = nullptr;
        REX::W32::DXGI_FORMAT indexFormat{};
        UINT indexOffset = 0;

        std::array<bool, kReplayVertexBufferSlots> vertexBufferValid{};
        std::array<ReplayVertexBufferState, kReplayVertexBufferSlots> vertexBuffers{};
    };

    struct ReplayMapResourceStats
    {
        struct KeyStats
        {
            std::uint64_t calls = 0;
            std::uint64_t ns = 0;
            std::uint64_t payloadWrites = 0;
            std::uint64_t payloadDuplicateWrites = 0;
        };

        REX::W32::D3D11_RESOURCE_DIMENSION dimension = REX::W32::D3D11_RESOURCE_DIMENSION_UNKNOWN;
        REX::W32::D3D11_MAP mapType = REX::W32::D3D11_MAP_READ;
        UINT mapFlags = 0;
        UINT subresource = 0;
        std::uint32_t byteWidth = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t depth = 0;
        std::uint32_t bindFlags = 0;
        std::uint32_t usage = 0;
        std::uint32_t cpuAccessFlags = 0;
        std::uint32_t format = 0;
        std::uint64_t mapCalls = 0;
        std::uint64_t unmapCalls = 0;
        std::uint64_t mapNs = 0;
        std::uint64_t unmapNs = 0;
        std::uint64_t maxMapNs = 0;
        std::uint64_t maxUnmapNs = 0;
        std::uint64_t payloadWrites = 0;
        std::uint64_t payloadDuplicateWrites = 0;
        std::uint64_t lastPayloadHash = 0;
        bool lastPayloadHashValid = false;
        std::unordered_map<std::uint32_t, KeyStats> keyStats;
        bool described = false;
    };

    struct ActiveReplayMapInfo
    {
        void* data = nullptr;
        std::uint32_t byteWidth = 0;
        std::uint32_t bindFlags = 0;
        REX::W32::D3D11_RESOURCE_DIMENSION dimension = REX::W32::D3D11_RESOURCE_DIMENSION_UNKNOWN;
        REX::W32::D3D11_MAP mapType = REX::W32::D3D11_MAP_READ;
        std::uint32_t key = UINT_MAX;
    };

    thread_local ReplaySRVCallCache tl_replaySRVCallCache;
    thread_local ReplayIACache tl_replayIACache;
    thread_local std::unordered_map<REX::W32::ID3D11Resource*, ReplayMapResourceStats> tl_replayMapStats;
    thread_local std::unordered_map<REX::W32::ID3D11Resource*, ActiveReplayMapInfo> tl_activeReplayMaps;
    thread_local std::unordered_set<std::uint64_t> tl_loggedReplayPayloadSamples;
    thread_local std::chrono::steady_clock::time_point tl_replayMapStatsStart = std::chrono::steady_clock::now();

    void ResetReplayStateCache()
    {
        tl_replaySRVCallCache = {};
        tl_replayIACache = {};
    }
}

std::uint64_t GetD3DDrawCallsLastFrame_Internal()
{
    return g_d3dDrawCallsLastFrame.load(std::memory_order_relaxed);
}


static DirectX::XMFLOAT3 ColorFromPackedRGB(std::uint32_t color)
{
    const float inv255 = 1.0f / 255.0f;
    return DirectX::XMFLOAT3(
        static_cast<float>((color >> 16) & 0xFF) * inv255,
        static_cast<float>((color >> 8) & 0xFF) * inv255,
        static_cast<float>(color & 0xFF) * inv255);
}

static const RE::INTERIOR_DATA* GetInteriorDataForDominantLight(RE::TESObjectCELL* cell)
{
    if (!cell || !cell->IsInterior()) {
        return nullptr;
    }

    if (cell->cellDataInterior) {
        return cell->cellDataInterior;
    }

    return cell->lightingTemplate ? std::addressof(cell->lightingTemplate->data) : nullptr;
}

static DirectX::XMFLOAT3 DirectionFromInteriorAngles(std::uint32_t directionalXY, std::uint32_t directionalZ)
{
    const float yaw = static_cast<float>(directionalXY & 0xFF) * (DirectX::XM_2PI / 255.0f);
    const float pitch = (static_cast<float>(directionalZ & 0xFF) / 255.0f - 0.5f) * DirectX::XM_PI;
    const float cosPitch = std::cos(pitch);

    return DirectX::XMFLOAT3(
        std::cos(yaw) * cosPitch,
        std::sin(yaw) * cosPitch,
        std::sin(pitch));
}

static DirectX::XMFLOAT3 DirectionFromNodeForward(const RE::NiAVObject* object)
{
    if (!object) {
        return DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    }

    const auto& row = object->world.rotate[1];
    const float lenSq = row.x * row.x + row.y * row.y + row.z * row.z;
    if (lenSq <= 1.0e-6f) {
        return DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    }

    const float invLen = 1.0f / std::sqrt(lenSq);
    return DirectX::XMFLOAT3(row.x * invLen, row.y * invLen, row.z * invLen);
}
// Global frame counter for slow updates
static uint32_t g_frameTick = 0;      // frame counter
// Global player values
static float    g_healthPerc = 1.0f; // value sampled 30 frames ago
static float    g_lastRad = 0.0f;   // value sampled 30 frames ago
static float    g_radDmg = 0.0f;    // calculated value send to the shader
static bool     g_haveRadBaseline = false;
// Global wind values
static float    g_windSpeed = 0.0f;     // value sampled 30 frames ago
static float    g_windAngle = 0.0f;     // value sampled 30 frames ago
static float    g_windTurbulence = 0.0f;// value sampled 30 frames ago
// Global combat flag
static bool     g_inCombat = false;    // value sampled 30 frames ago
// Global interior flag
static bool     g_inInterior = false; // value sampled 30 frames ago
void ResetPlayerRadDamageTracking()
{
    g_lastRad = 0.0f;
    g_radDmg = 0.0f;
    g_haveRadBaseline = false;
}


static int32_t GetWeatherClassification(RE::TESWeather* weather)
{
    if (!weather) {
        return -1;
    }

    const auto flags = weather->weatherData[11];
    if ((flags & 1) != 0) return 0; // Pleasant
    if ((flags & 2) != 0) return 1; // Cloudy
    if ((flags & 4) != 0) return 2; // Rainy
    if ((flags & 8) != 0) return 3; // Snow

    return -1;
}

static DirectX::XMFLOAT4 TransformRotationRow(const RE::NiTransform& transform, std::size_t row)
{
    const auto& r = transform.rotate[row];
    return { r.x, r.y, r.z, r.w };
}

static DirectX::XMFLOAT4 TransformTranslationRow(const RE::NiTransform& transform)
{
    return { transform.translate.x, transform.translate.y, transform.translate.z, transform.scale };
}

static void StoreCameraTransform(
    const RE::NiTransform& transform,
    DirectX::XMFLOAT4& row0,
    DirectX::XMFLOAT4& row1,
    DirectX::XMFLOAT4& row2,
    DirectX::XMFLOAT4& row3)
{
    row0 = TransformRotationRow(transform, 0);
    row1 = TransformRotationRow(transform, 1);
    row2 = TransformRotationRow(transform, 2);
    row3 = TransformTranslationRow(transform);
}

static bool IsNodeInHierarchy(const RE::NiAVObject* object, const RE::NiAVObject* root)
{
    for (auto* current = object; current; current = current->parent) {
        if (current == root) {
            return true;
        }
    }

    return false;
}

static const RE::NiAVObject* GetPlayerCameraRoot()
{
    auto* playerCamera = RE::PlayerCamera::GetSingleton();
    if (!playerCamera) {
        return nullptr;
    }

    if (playerCamera->currentState && playerCamera->currentState->camera && playerCamera->currentState->camera->cameraRoot) {
        return playerCamera->currentState->camera->cameraRoot.get();
    }

    return playerCamera->cameraRoot.get();
}

static const RE::BSGraphics::CameraStateData* SelectGameplayCameraState(const RE::BSGraphics::State& gfxState)
{
    const auto* worldCamera = RE::Main::WorldRootCamera();
    if (worldCamera) {
        for (const auto& cachedCamera : gfxState.cameraDataCache) {
            if (cachedCamera.referenceCamera == worldCamera) {
                return std::addressof(cachedCamera);
            }
        }
    }

    const auto* playerCameraRoot = GetPlayerCameraRoot();
    if (playerCameraRoot) {
        for (const auto& cachedCamera : gfxState.cameraDataCache) {
            if (cachedCamera.referenceCamera && IsNodeInHierarchy(cachedCamera.referenceCamera, playerCameraRoot)) {
                return std::addressof(cachedCamera);
            }
        }
    }

    return std::addressof(gfxState.cameraState);
}
struct SHRows
{
    DirectX::XMFLOAT4 r;
    DirectX::XMFLOAT4 g;
    DirectX::XMFLOAT4 b;
};

static DirectX::XMFLOAT3 Mul3x3(const float m[3][3], DirectX::XMFLOAT3 v)
{
    return {
        m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
        m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
        m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z
    };
}

static DirectX::XMFLOAT4 PackChannel(
    float xp, float xn,
    float yp, float yn,
    float zp, float zn,
    const float skyToNormalBasis[3][3])
{
    // No pow here.
    DirectX::XMFLOAT3 gradientSky{
        (xp - xn) * 0.5f,
        (yp - yn) * 0.5f,
        (zp - zn) * 0.5f
    };

    DirectX::XMFLOAT3 gradientNormal =
        Mul3x3(skyToNormalBasis, gradientSky);

    float mean = (xp + xn + yp + yn + zp + zn) * (1.0f / 6.0f);

    return {
        gradientNormal.x,
        gradientNormal.y,
        gradientNormal.z,
        mean
    };
}

// Update the custom buffer for shaders
void UpdateCustomBuffer_Internal() {
    // Fill the custom buffer data structure with current frame info
    static LARGE_INTEGER frequency = {};
    static LARGE_INTEGER lastFrameTime = {};
    static LARGE_INTEGER startTime = {};
    static bool initialized = false;
    static bool firstFrame = true; // Flag to initialize smoothedFPS properly
    static float smoothedFPS = 0.0f;
    static uint32_t frameCounter = 0;
    // Initialize timing on first call
    if (!initialized) {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&startTime);
        lastFrameTime = startTime;
        initialized = true;
    }
    // Get current time
    LARGE_INTEGER currentTime;
    QueryPerformanceCounter(&currentTime);
    // Calculate delta time
    float deltaTime = static_cast<float>(currentTime.QuadPart - lastFrameTime.QuadPart) / static_cast<float>(frequency.QuadPart);
    lastFrameTime = currentTime;
    // Calculate total elapsed time
    float totalTime = static_cast<float>(currentTime.QuadPart - startTime.QuadPart) / static_cast<float>(frequency.QuadPart);
    // Calculate Instant FPS
    // We use a small epsilon (0.0001) to prevent any potential division by zero
    float instantFPS = (deltaTime > 0.0001f) ? (1.0f / deltaTime) : 0.0f;
    // Smooth the FPS
    if (firstFrame && instantFPS > 0.0f) {
        // Use Instant FPS at start
        smoothedFPS = instantFPS;
        firstFrame = false;
    } else {
        // Standard exponential moving average for all subsequent frames
        smoothedFPS = smoothedFPS * 0.95f + instantFPS * 0.05f;
    }
    // Get screen resolution from the main render target (kMain = 3).
    float resX = 1920.0f, resY = 1080.0f;
    if (g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture) {
        REX::W32::D3D11_TEXTURE2D_DESC desc{};
        g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture->GetDesc(&desc);
        resX = static_cast<float>(desc.width);
        resY = static_cast<float>(desc.height);
    }
    // Get mouse position (normalized to 0.0 - 1.0)
    POINT mousePos{};
    GetCursorPos(&mousePos);
    ScreenToClient(GetActiveWindow(), &mousePos);
    float mousePosX = static_cast<float>(mousePos.x) / resX;
    float mousePosY = static_cast<float>(mousePos.y) / resY;
    // Get the viewport data to extract the camera position and forward vector
    auto gfxState = RE::BSGraphics::State::GetSingleton();
    auto& camState = *SelectGameplayCameraState(gfxState); // CameraStateData
    auto& camView  = camState.camViewData;        // viewMat, viewDir, viewUp, viewRight, viewPort
    // viewport
    auto vp = camView.viewPort;
    float vpX = vp.left, vpY = vp.top;
    float vpW = vp.right - vp.left, vpH = vp.bottom - vp.top;
    // forward vector -> yaw/pitch
    auto vd = camView.viewDir;
    float vx = vd.m128_f32[0], vy = vd.m128_f32[1], vz = vd.m128_f32[2];
    // BGS builds a rotation-only view matrix. Its inverse translation is the
    // camera-relative origin; absolute translation lives in CameraStateData's
    // *PosAdjust fields and is injected separately below.
    auto& VM = camView.viewMat; // __m128 viewMat[4]
    DirectX::XMMATRIX view = DirectX::XMMATRIX(VM[0], VM[1], VM[2], VM[3]);
    DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
    float camX = DirectX::XMVectorGetX(invView.r[3]);
    float camY = DirectX::XMVectorGetY(invView.r[3]);
    float camZ = DirectX::XMVectorGetZ(invView.r[3]);
    if (!g_player)
        g_player = RE::PlayerCharacter::GetSingleton();
    if (!g_actorValueInfo)
        g_actorValueInfo = RE::ActorValue::GetSingleton();
    if (!g_sky)
        g_sky = RE::Sky::GetSingleton();
    // Slower updates every 30 frames for expensive queries
    if (++g_frameTick >= 30) {
        g_frameTick = 0;
        // Get the player health percentage, clamped to [0,1]
        if (g_player)
            g_healthPerc = std::clamp(g_player->extraList->GetHealthPerc(), 0.0f, 1.0f);
        // Get the current incoming radiation damage. Existing accumulated rads are
        // baseline state and should not drive the shader after loading a save.
        float rawRad = 0.0f;
        if (g_player && g_actorValueInfo) {
            rawRad = g_player->GetActorValue(*g_actorValueInfo->rads);
        }
        if (!g_haveRadBaseline) {
            g_lastRad = rawRad;
            g_radDmg = 0.0f;
            g_haveRadBaseline = true;
        } else {
            const float diffRad = rawRad - g_lastRad;
            g_lastRad = rawRad;
            if (g_player && g_player->IsTakingRadDamageFromActiveEffect() && diffRad > 0.0f) {
                g_radDmg = diffRad;
            } else {
                // Decay radiation so the effect fades when the active damage stops.
                g_radDmg = (std::max)(g_radDmg - 0.1f, 0.0f);
            }
        }
        // Wind data from the sky (for foliage shaders)
        if (g_sky) {
            g_windSpeed = g_sky->windSpeed;
            g_windAngle = g_sky->windAngle;
            g_windTurbulence = g_sky->windTurbulence;
        }
        // Check if the player is in combat
        if (g_player)
            g_inCombat = g_player->IsInCombat();
        // Check if the current Cell is an interior or exterior for sky-related shader effects
        if (g_player && g_player->GetParentCell()) {
            g_inInterior = g_player->GetParentCell()->IsInterior();
        }
    }
    // Get the Projection Inverse matrix to extract the camera FOV
    auto& PM = camView.projMat; // __m128 projMat[4]
    DirectX::XMMATRIX proj = DirectX::XMMATRIX(PM[0], PM[1], PM[2], PM[3]);   // load into XMMATRIX
    DirectX::XMMATRIX invProj = DirectX::XMMatrixInverse(nullptr, proj);
    // Use the game-provided view-projection matrix directly.
    auto& VPM = camView.viewProjMat;
    DirectX::XMMATRIX viewProj = DirectX::XMMATRIX(VPM[0], VPM[1], VPM[2], VPM[3]);
    // BSGraphics::ViewData+0x190 is the renderer-maintained previous
    // unjittered view-projection. It is paired with
    // CameraStateData::previousPosAdjust (+0x228). Live OG 1.10.163 memory
    // confirms both fields for the selected gameplay camera.
    auto& previousVPM = camView.previousViewProjUnjittered;
    DirectX::XMMATRIX previousViewProj = DirectX::XMMATRIX(
        previousVPM[0],
        previousVPM[1],
        previousVPM[2],
        previousVPM[3]);
    float timeOfDay = 0.0f;
    float weatherTransition = 0.0f;
    uint32_t currentWeatherID = 0;
    uint32_t outgoingWeatherID = 0;
    uint32_t currentLocationID = 0;
    uint32_t worldSpaceID = 0;
    uint32_t skyMode = 0;
    int32_t currentWeatherClass = -1;
    int32_t outgoingWeatherClass = -1;
    auto* parentCell = g_player ? g_player->GetParentCell() : nullptr;
    DirectX::XMFLOAT3 sunColor(1.0f, 1.0f, 1.0f);
    DirectX::XMFLOAT3 sunDir(0.0f, 0.0f, 0.0f);
    float sunValid = 0.0f;
    if (g_sky) {
        timeOfDay = g_sky->currentGameHour;
        skyMode = g_sky->mode.underlying();
        const bool validInterior = parentCell && parentCell->IsInterior() && parentCell->lightingTemplate;
        if (validInterior) {
            weatherTransition = g_sky->lightingTransition == 0.0f ? 1.0f : g_sky->lightingTransition;
            currentWeatherID = parentCell->lightingTemplate->GetFormID();
            outgoingWeatherID = 0;
        } else {
            weatherTransition = g_sky->currentWeatherPct;
            if (g_sky->currentWeather) {
                currentWeatherID = g_sky->currentWeather->GetFormID();
                currentWeatherClass = GetWeatherClassification(g_sky->currentWeather);
            }
            if (g_sky->lastWeather) {
                outgoingWeatherID = g_sky->lastWeather->GetFormID();
                outgoingWeatherClass = GetWeatherClassification(g_sky->lastWeather);
            }
        }
    }
    if (parentCell && parentCell->IsInterior()) {
        if (const auto* interiorData = GetInteriorDataForDominantLight(parentCell)) {
            sunColor = ColorFromPackedRGB(interiorData->directional);
            sunDir = DirectionFromInteriorAngles(interiorData->directionalXY, interiorData->directionalZ);
            sunValid = std::clamp(interiorData->directionalFade, 0.0f, 1.0f);
        }
    } else if (g_sky) {
        // Sky::GetSunLightColor is a trivial return of Sky+0x0D8, i.e.
        // skyColor[4]. Use the already-blended Sky value instead of sampling
        // TESWeather colorData manually.
        const auto& c = g_sky->skyColor[4];
        sunColor = DirectX::XMFLOAT3(c.r, c.g, c.b);
        if (g_sky->sun && g_sky->sun->light) {
            sunDir = DirectionFromNodeForward(reinterpret_cast<const RE::NiAVObject*>(g_sky->sun->light.get()));
        }
        sunValid = 1.0f;
    }
    if (g_player) {
        if (g_player->currentLocation) {
            currentLocationID = g_player->currentLocation->GetFormID();
        }
        if (g_player->cachedWorldspace) {
            worldSpaceID = g_player->cachedWorldspace->GetFormID();
        }
    }
    // Get a random number each frame
    float randomValue = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
    // Fill the custom buffer data structure
    g_customBufferData.time     = totalTime;
    g_customBufferData.delta    = deltaTime;
    g_customBufferData.dayCycle = timeOfDay / 24.0f;
    g_customBufferData.frame    = static_cast<float>(frameCounter++);
    g_customBufferData.fps      = smoothedFPS;
    g_customBufferData.resX     = resX;
    g_customBufferData.resY     = resY;
    g_customBufferData.mouseX   = static_cast<float>(mousePos.x) / resX;
    g_customBufferData.mouseY   = static_cast<float>(mousePos.y) / resY;
    g_customBufferData.windSpeed = g_windSpeed;
    g_customBufferData.windAngle = g_windAngle;
    g_customBufferData.windTurb  = g_windTurbulence;
    g_customBufferData.vpLeft   = vp.left;
    g_customBufferData.vpTop    = vp.top;
    g_customBufferData.vpWidth  = vp.right - vp.left;
    g_customBufferData.vpHeight = vp.bottom - vp.top;
    g_customBufferData.camX     = camX;
    g_customBufferData.camY     = camY;
    g_customBufferData.camZ     = camZ;
    g_customBufferData.pRadDmg  = g_radDmg;
    g_customBufferData.viewDirX = vd.m128_f32[0];
    g_customBufferData.viewDirY = vd.m128_f32[1];
    g_customBufferData.viewDirZ = vd.m128_f32[2];
    g_customBufferData.pHealthPerc = g_healthPerc;
    g_customBufferData.g_SunR = sunColor.x;
    g_customBufferData.g_SunG = sunColor.y;
    g_customBufferData.g_SunB = sunColor.z;
    g_customBufferData.g_SunDirX = sunDir.x;
    g_customBufferData.g_SunDirY = sunDir.y;
    g_customBufferData.g_SunDirZ = sunDir.z;
    g_customBufferData.g_SunValid = sunValid;
    g_customBufferData.g_SunPadding = 0.0f;

    auto Normalize3 = [](DirectX::XMFLOAT3 v) {
        const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
        if (lenSq <= 1.0e-8f) {
            return DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        }

        const float invLen = 1.0f / std::sqrt(lenSq);
        return DirectX::XMFLOAT3(v.x * invLen, v.y * invLen, v.z * invLen);
    };

    auto FromXM = [](DirectX::XMVECTOR v) {
        return DirectX::XMFLOAT3(
            DirectX::XMVectorGetX(v),
            DirectX::XMVectorGetY(v),
            DirectX::XMVectorGetZ(v));
    };

    // View-space L1 SH packing ? derived basis matching engine's cb2[6..8].
    //
    // Empirically reverse-engineered from CAEE89E9/B9652BF6 capture data
    // across 5+ camera orientations: cb2[6..8] = M @ (dac_diff/2) where
    //   M[0] = normalize(world_up × camera_forward)
    //   M[1] = -normalize(camera_forward × M[0])
    //   M[2] = -camera_forward
    // Plus cb2.w = mean of all 6 dac values (DC term).
    //
    // Verified bit-exact (err < 1e-6) for the engine's own cb2[6..8] given
    // current camera basis. See AmbientLightFix.md for derivation.
    //
    // KNOWN ISSUE: per-pixel output still drifts visibly with camera
    // rotation despite matching engine's cb2 packing. The dot product
    // should be rotation-invariant when both cb2 and gbuffer normal
    // rotate together ? but we have evidence the gbuffer normal in
    // Fallout 4 is in WORLD space (OG's existing math requires this for
    // cascade shadows + world sun direction). Engine view-space cb2 ×
    // world-space N gives camera-dependent results. This contradicts the
    // engine's B9652BF6 producing stable output, suggesting some part of
    // our understanding is incomplete (possibly: gbuffer normal IS view-
    // space but OG transforms it implicitly through some path we haven't
    // identified, OR engine has a separate cb2[1] convention per shader).
    DirectX::XMFLOAT3 cameraForwardWorld = Normalize3(FromXM(camView.viewDir));
    DirectX::XMFLOAT3 worldUp(0.0f, 0.0f, 1.0f);
    DirectX::XMFLOAT3 row0, row1, row2;
    {
        DirectX::XMFLOAT3 cross0(
            worldUp.y * cameraForwardWorld.z - worldUp.z * cameraForwardWorld.y,
            worldUp.z * cameraForwardWorld.x - worldUp.x * cameraForwardWorld.z,
            worldUp.x * cameraForwardWorld.y - worldUp.y * cameraForwardWorld.x);
        float cross0Len = std::sqrt(cross0.x * cross0.x + cross0.y * cross0.y + cross0.z * cross0.z);
        if (cross0Len > 1e-4f) {
            row0 = DirectX::XMFLOAT3(cross0.x / cross0Len, cross0.y / cross0Len, cross0.z / cross0Len);
        } else {
            row0 = DirectX::XMFLOAT3(1.0f, 0.0f, 0.0f);
        }
        DirectX::XMFLOAT3 cross1(
            cameraForwardWorld.y * row0.z - cameraForwardWorld.z * row0.y,
            cameraForwardWorld.z * row0.x - cameraForwardWorld.x * row0.z,
            cameraForwardWorld.x * row0.y - cameraForwardWorld.y * row0.x);
        float cross1Len = std::sqrt(cross1.x * cross1.x + cross1.y * cross1.y + cross1.z * cross1.z);
        if (cross1Len > 1e-6f) {
            row1 = DirectX::XMFLOAT3(-cross1.x / cross1Len, -cross1.y / cross1Len, -cross1.z / cross1Len);
        } else {
            row1 = DirectX::XMFLOAT3(0.0f, 0.0f, -1.0f);
        }
        row2 = DirectX::XMFLOAT3(-cameraForwardWorld.x, -cameraForwardWorld.y, -cameraForwardWorld.z);
    }
    float skyToNormalBasis[3][3] = {
        { row0.x, row0.y, row0.z },
        { row1.x, row1.y, row1.z },
        { row2.x, row2.y, row2.z },
    };
    DirectX::XMFLOAT4 shR(0.0f, 0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT4 shG(0.0f, 0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT4 shB(0.0f, 0.0f, 0.0f, 0.0f);
    if (g_sky) {
        const auto& dac = g_sky->directionalAmbientColorsA;

        shR = PackChannel(
            dac[0][0].r, dac[0][1].r,
            dac[1][0].r, dac[1][1].r,
            dac[2][0].r, dac[2][1].r,
            skyToNormalBasis);

        shG = PackChannel(
            dac[0][0].g, dac[0][1].g,
            dac[1][0].g, dac[1][1].g,
            dac[2][0].g, dac[2][1].g,
            skyToNormalBasis);

        shB = PackChannel(
            dac[0][0].b, dac[0][1].b,
            dac[1][0].b, dac[1][1].b,
            dac[2][0].b, dac[2][1].b,
            skyToNormalBasis);
    }
    g_customBufferData.g_SH_R = shR;
    g_customBufferData.g_SH_G = shG;
    g_customBufferData.g_SH_B = shB;
    g_customBufferData.g_CurrentCameraPositionAdjust = {
        camState.currentPosAdjust.x,
        camState.currentPosAdjust.y,
        camState.currentPosAdjust.z,
        0.0f
    };
    g_customBufferData.g_PreviousCameraPositionAdjust = {
        camState.previousPosAdjust.x,
        camState.previousPosAdjust.y,
        camState.previousPosAdjust.z,
        0.0f
    };

    // Resolve the projection domain using the same precedence as the
    // upscaler-aware deferred shadow path:
    //   physical proxy allocation > native DRS ratio > reduced viewport.
    // Fallout's depth pyramid (RT39) is not proxy-swapped by the upscaler, so
    // shaders need this extent separately from any individual texture size.
    const float displayW = gfxState.screenWidth > 0
        ? static_cast<float>(gfxState.screenWidth)
        : resX;
    const float displayH = gfxState.screenHeight > 0
        ? static_cast<float>(gfxState.screenHeight)
        : resY;
    const auto renderTargetManager =
        RE::BSGraphics::RenderTargetManager::GetSingleton();
    const float dynamicWidthRatio =
        renderTargetManager.dynamicWidthRatio > 0.0f &&
        renderTargetManager.dynamicWidthRatio <= 1.0f
            ? renderTargetManager.dynamicWidthRatio
            : 1.0f;
    const float dynamicHeightRatio =
        renderTargetManager.dynamicHeightRatio > 0.0f &&
        renderTargetManager.dynamicHeightRatio <= 1.0f
            ? renderTargetManager.dynamicHeightRatio
            : 1.0f;
    constexpr float kMinimumRenderExtent = 16.0f;
    const bool physicalExtentValid =
        std::isfinite(resX) &&
        std::isfinite(resY) &&
        resX >= kMinimumRenderExtent &&
        resY >= kMinimumRenderExtent;
    const bool viewportExtentValid =
        std::isfinite(vpW) &&
        std::isfinite(vpH) &&
        vpW >= kMinimumRenderExtent &&
        vpH >= kMinimumRenderExtent &&
        vpW <= displayW + 1.0f &&
        vpH <= displayH + 1.0f;
    const bool proxyAllocation =
        physicalExtentValid &&
        (resX + 0.5f < displayW || resY + 0.5f < displayH);
    const bool dynamicRatio =
        dynamicWidthRatio < 0.999f || dynamicHeightRatio < 0.999f;
    const bool reducedViewport =
        viewportExtentValid &&
        (vpW + 0.5f < displayW || vpH + 0.5f < displayH);
    struct StableRenderDomain
    {
        float renderW = 0.0f;
        float renderH = 0.0f;
        float displayW = 0.0f;
        float displayH = 0.0f;
    };
    static StableRenderDomain stableRenderDomain{};
    float renderW = resX;
    float renderH = resY;
    if (proxyAllocation) {
        renderW = resX;
        renderH = resY;
    } else if (dynamicRatio) {
        renderW = (std::max)(
            1.0f, std::floor(displayW * dynamicWidthRatio));
        renderH = (std::max)(
            1.0f, std::floor(displayH * dynamicHeightRatio));
    } else if (reducedViewport) {
        renderW = std::round(vpW);
        renderH = std::round(vpH);
    } else if (!viewportExtentValid) {
        // Loading screens and image-space transitions can publish a camera
        // viewport with one zero-sized axis while the upscaler temporarily
        // forces the DRS ratios to 1.0. Treating that as a reduced viewport
        // collapsed render-domain custom resources to 1x1. Retain the last
        // trustworthy domain for the same display instead.
        const bool sameDisplay =
            std::abs(stableRenderDomain.displayW - displayW) < 0.5f &&
            std::abs(stableRenderDomain.displayH - displayH) < 0.5f;
        if (sameDisplay &&
            stableRenderDomain.renderW >= kMinimumRenderExtent &&
            stableRenderDomain.renderH >= kMinimumRenderExtent) {
            renderW = stableRenderDomain.renderW;
            renderH = stableRenderDomain.renderH;
        } else {
            renderW = physicalExtentValid ? resX : displayW;
            renderH = physicalExtentValid ? resY : displayH;
        }
    }
    if (renderW < kMinimumRenderExtent ||
        renderH < kMinimumRenderExtent ||
        !std::isfinite(renderW) ||
        !std::isfinite(renderH)) {
        const bool sameDisplay =
            std::abs(stableRenderDomain.displayW - displayW) < 0.5f &&
            std::abs(stableRenderDomain.displayH - displayH) < 0.5f;
        if (sameDisplay &&
            stableRenderDomain.renderW >= kMinimumRenderExtent &&
            stableRenderDomain.renderH >= kMinimumRenderExtent) {
            renderW = stableRenderDomain.renderW;
            renderH = stableRenderDomain.renderH;
        } else {
            renderW = physicalExtentValid ? resX : displayW;
            renderH = physicalExtentValid ? resY : displayH;
        }
    }
    if (renderW >= kMinimumRenderExtent &&
        renderH >= kMinimumRenderExtent) {
        stableRenderDomain.renderW = renderW;
        stableRenderDomain.renderH = renderH;
        stableRenderDomain.displayW = displayW;
        stableRenderDomain.displayH = displayH;
    }
    g_customBufferData.g_RenderInfo = {
        renderW,
        renderH,
        displayW,
        displayH
    };

    DirectX::XMStoreFloat4(&g_customBufferData.g_InvProjRow0, invProj.r[0]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvProjRow1, invProj.r[1]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvProjRow2, invProj.r[2]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvProjRow3, invProj.r[3]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvViewRow0, invView.r[0]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvViewRow1, invView.r[1]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvViewRow2, invView.r[2]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_InvViewRow3, invView.r[3]);
    g_customBufferData.random  = randomValue;
    g_customBufferData.inCombat = g_inCombat ? 1.0f : 0.0f;
    g_customBufferData.inInterior = g_inInterior ? 1.0f : 0.0f;
    g_customBufferData._padding = 0.0f; // just in case, to avoid any potential uninitialized data issues in shaders
    DirectX::XMStoreFloat4(
        &g_customBufferData.g_PrevViewProjRow0,
        previousViewProj.r[0]);
    DirectX::XMStoreFloat4(
        &g_customBufferData.g_PrevViewProjRow1,
        previousViewProj.r[1]);
    DirectX::XMStoreFloat4(
        &g_customBufferData.g_PrevViewProjRow2,
        previousViewProj.r[2]);
    DirectX::XMStoreFloat4(
        &g_customBufferData.g_PrevViewProjRow3,
        previousViewProj.r[3]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_ViewProjRow0, viewProj.r[0]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_ViewProjRow1, viewProj.r[1]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_ViewProjRow2, viewProj.r[2]);
    DirectX::XMStoreFloat4(&g_customBufferData.g_ViewProjRow3, viewProj.r[3]);
    g_customBufferData.timeOfDay = timeOfDay;
    g_customBufferData.weatherTransition = weatherTransition;
    g_customBufferData.currentWeatherID = currentWeatherID;
    g_customBufferData.outgoingWeatherID = outgoingWeatherID;
    g_customBufferData.currentLocationID = currentLocationID;
    g_customBufferData.worldSpaceID = worldSpaceID;
    g_customBufferData.skyMode = skyMode;
    g_customBufferData.currentWeatherClass = currentWeatherClass;
    g_customBufferData.outgoingWeatherClass = outgoingWeatherClass;
    g_customBufferData.enbPadding0 = 0.0f;
    g_customBufferData.enbPadding1 = 0.0f;
    g_customBufferData.enbPadding2 = 0.0f;
    if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); playerCamera && playerCamera->cameraRoot) {
        const auto* cameraNode = playerCamera->cameraRoot.get();
        StoreCameraTransform(cameraNode->local, g_customBufferData.cameraLocalRow0, g_customBufferData.cameraLocalRow1, g_customBufferData.cameraLocalRow2, g_customBufferData.cameraLocalRow3);
        StoreCameraTransform(cameraNode->world, g_customBufferData.cameraWorldRow0, g_customBufferData.cameraWorldRow1, g_customBufferData.cameraWorldRow2, g_customBufferData.cameraWorldRow3);
        StoreCameraTransform(cameraNode->previousWorld, g_customBufferData.cameraPreviousWorldRow0, g_customBufferData.cameraPreviousWorldRow1, g_customBufferData.cameraPreviousWorldRow2, g_customBufferData.cameraPreviousWorldRow3);
    } else {
        StoreCameraTransform(RE::NiTransform::IDENTITY, g_customBufferData.cameraLocalRow0, g_customBufferData.cameraLocalRow1, g_customBufferData.cameraLocalRow2, g_customBufferData.cameraLocalRow3);
        StoreCameraTransform(RE::NiTransform::IDENTITY, g_customBufferData.cameraWorldRow0, g_customBufferData.cameraWorldRow1, g_customBufferData.cameraWorldRow2, g_customBufferData.cameraWorldRow3);
        StoreCameraTransform(RE::NiTransform::IDENTITY, g_customBufferData.cameraPreviousWorldRow0, g_customBufferData.cameraPreviousWorldRow1, g_customBufferData.cameraPreviousWorldRow2, g_customBufferData.cameraPreviousWorldRow3);
    }
    if (g_sky) {
        g_customBufferData.g_FogDistances0 = DirectX::XMFLOAT4(
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kNear)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kFar)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kWaterNear)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kWaterFar)]);
        g_customBufferData.g_FogDistances1 = DirectX::XMFLOAT4(
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kHeightMid)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kHeightRange)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kFarHeightMid)],
            g_sky->fogDistances[static_cast<size_t>(RE::Sky::FogDistance::kFarHeightRange)]);
        g_customBufferData.g_FogParams = DirectX::XMFLOAT4(
            g_sky->fogHeight,
            g_sky->fogPower,
            g_sky->fogClamp,
            g_sky->fogHighDensityScale);
    } else {
        g_customBufferData.g_FogDistances0 = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        g_customBufferData.g_FogDistances1 = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        g_customBufferData.g_FogParams     = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    // Per-weather blended fog color requires sampling currentWeather + lastWeather
    // colorData[][] arrays at the current time-of-day and blending by
    // currentWeatherPct. Left at zero for now -- shaders should treat (0,0,0,0)
    // as "no engine-supplied color, use Values.ini knobs".
    g_customBufferData.g_FogColor = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    // Pack Values.ini shader settings into separate 16-byte elements to avoid the structured-buffer stride limit.
    ShaderResources::PackModularShaderValues(g_shaderSettings);

    // Create or update our custom buffer resource views with the new data.
    if (!g_rendererData) {
        g_rendererData = RE::BSGraphics::GetRendererData();
        if (!g_rendererData) {
            REX::WARN("UpdateCustomBuffer_Internal: Cannot update custom buffer: renderer data not ready");
            return;
        }
    }

    ShaderResources::EnsureInjectedShaderResourceViews(g_rendererData->device);
    ShaderResources::UpdateInjectedShaderResourceViews(g_rendererData->context);
}

void RefreshCustomBufferForCustomPass()
{
    if (!CUSTOMBUFFER_ON) {
        return;
    }

    bool expected = false;
    if (!g_customBufferRefreshedBeforePresent.compare_exchange_strong(
            expected,
            true,
            std::memory_order_relaxed)) {
        return;
    }
    UpdateCustomBuffer_Internal();
}

// --- D3D11 hook handlers ---

void D3D11OnPresent_Internal()
{
    g_d3dDrawCallsLastFrame.store(
        g_d3dDrawCallsThisFrame.exchange(0, std::memory_order_relaxed),
        std::memory_order_relaxed);

    // Light-tracker frame boundary: finalize the previous capture (if any),
    // poll Numpad * for new arms, open the next capture file. No-op when
    // DEVELOPMENT is off.
    LightTracker::Tick();
    const bool refreshedAtCustomPass =
        g_customBufferRefreshedBeforePresent.exchange(
            false,
            std::memory_order_relaxed);
    if (CUSTOMBUFFER_ON && !refreshedAtCustomPass) {
        UpdateCustomBuffer_Internal();
    }
    // Custom-pass per-frame work: allocate resources, run any AtPresent passes,
    // ping-pong, advance frame counter. Done after the booster CB update so
    // GFXInjected[0] is fresh for any AtPresent pass.
    if (SHADERENGINE_EFFECTS_ON && g_rendererData && g_rendererData->context) {
        CustomPass::g_registry.OnFramePresent(g_rendererData->context);
        // GPU scalar probes: dispatch each probe's 1-thread CS and read back
        // the previous frame's value. GFXInjected and modular value buffers
        // are already fresh from UpdateCustomBuffer_Internal above, so the
        // probes see the same per-frame data as regular customPass shaders.
        GpuScalar::OnFramePresent(g_rendererData->context);
    }
    UIRenderFrame();
}

void D3D11OnPSSetShaderResources_Internal(REX::W32::ID3D11DeviceContext* context)
{
    if (g_customPassRendering) {
        return;
    }
    if (SHADERENGINE_EFFECTS_ON &&
        ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind() &&
        !g_bindingInjectedPixelResources) {
        ShaderResources::BindInjectedPixelShaderResources(context);
    }
}

void D3D11OnOMSetRenderTargets_Internal(
    REX::W32::ID3D11DeviceContext*,
    UINT numViews,
    REX::W32::ID3D11RenderTargetView* const* renderTargetViews,
    REX::W32::ID3D11DepthStencilView* depthStencilView)
{
    ShaderResources::TrackOMRenderTargets(numViews, renderTargetViews, depthStencilView);
}

bool D3D11ShouldSuppressClearDepthStencilView_Internal(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11DepthStencilView* depthStencilView,
    UINT clearFlags,
    FLOAT depth,
    UINT8 stencil)
{
    return SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON &&
        ShadowTelemetry::HandleShadowCacheClearDepthStencilView(
            context, depthStencilView, clearFlags, depth, stencil);
}

void D3D11OnDraw_Internal(REX::W32::ID3D11DeviceContext* context, const char* source)
{
    if (g_customPassRendering) {
        return;
    }
    g_d3dDrawCallsThisFrame.fetch_add(1, std::memory_order_relaxed);
    if (PhaseTelemetry::g_mode.load(std::memory_order_relaxed) == PhaseTelemetry::Mode::On) {
        PhaseTelemetry::OnD3DDraw();
    }
    if (ShadowTelemetry::g_mode.load(std::memory_order_relaxed) == ShadowTelemetry::Mode::On) {
        ShadowTelemetry::OnD3DDraw();
    }
    // BindDrawTagForCurrentDraw(context);
    if (!SHADERENGINE_EFFECTS_ON) {
        return;
    }
    FireArmedCustomPassDrawBatch(context, source);
    if (ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind()) {
        ShaderResources::BindInjectedPixelShaderResources(context);
    }
}

template <class Fn>
auto ProfileCommandBufferD3DCall(PhaseTelemetry::CommandBufferD3DCallKind kind, Fn&& fn)
{
#if !SHADERENGINE_ENABLE_PHASE_TELEMETRY
    (void)kind;
    return fn();
#else
    if (!PhaseTelemetry::IsInCommandBufferReplay()) {
        return fn();
    }

    const auto t0 = std::chrono::steady_clock::now();
    if constexpr (std::is_void_v<std::invoke_result_t<Fn>>) {
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        PhaseTelemetry::OnCommandBufferD3DCall(
            kind,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    } else {
        auto result = fn();
        const auto t1 = std::chrono::steady_clock::now();
        PhaseTelemetry::OnCommandBufferD3DCall(
            kind,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        return result;
    }
#endif
}

const char* D3D11ResourceDimensionName(REX::W32::D3D11_RESOURCE_DIMENSION dimension) noexcept
{
    switch (dimension) {
    case REX::W32::D3D11_RESOURCE_DIMENSION_BUFFER:    return "Buffer";
    case REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE1D: return "Texture1D";
    case REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE2D: return "Texture2D";
    case REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE3D: return "Texture3D";
    default:                                          return "Unknown";
    }
}

const char* D3D11MapTypeName(REX::W32::D3D11_MAP mapType) noexcept
{
    switch (mapType) {
    case REX::W32::D3D11_MAP_READ:             return "READ";
    case REX::W32::D3D11_MAP_WRITE:            return "WRITE";
    case REX::W32::D3D11_MAP_READ_WRITE:       return "READ_WRITE";
    case REX::W32::D3D11_MAP_WRITE_DISCARD:    return "WRITE_DISCARD";
    case REX::W32::D3D11_MAP_WRITE_NO_OVERWRITE: return "WRITE_NO_OVERWRITE";
    default:                                   return "?";
    }
}

void DescribeReplayMapResource(REX::W32::ID3D11Resource* resource, ReplayMapResourceStats& stats)
{
    if (!resource || stats.described) {
        return;
    }

    resource->GetType(std::addressof(stats.dimension));
    switch (stats.dimension) {
    case REX::W32::D3D11_RESOURCE_DIMENSION_BUFFER: {
        REX::W32::D3D11_BUFFER_DESC desc{};
        static_cast<REX::W32::ID3D11Buffer*>(resource)->GetDesc(std::addressof(desc));
        stats.byteWidth = desc.byteWidth;
        stats.bindFlags = desc.bindFlags;
        stats.usage = static_cast<std::uint32_t>(desc.usage);
        stats.cpuAccessFlags = desc.cpuAccessFlags;
        break;
    }
    case REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        REX::W32::D3D11_TEXTURE1D_DESC desc{};
        static_cast<REX::W32::ID3D11Texture1D*>(resource)->GetDesc(std::addressof(desc));
        stats.width = desc.width;
        stats.format = static_cast<std::uint32_t>(desc.format);
        stats.bindFlags = desc.bindFlags;
        stats.usage = static_cast<std::uint32_t>(desc.usage);
        stats.cpuAccessFlags = desc.cpuAccessFlags;
        break;
    }
    case REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        REX::W32::D3D11_TEXTURE2D_DESC desc{};
        static_cast<REX::W32::ID3D11Texture2D*>(resource)->GetDesc(std::addressof(desc));
        stats.width = desc.width;
        stats.height = desc.height;
        stats.format = static_cast<std::uint32_t>(desc.format);
        stats.bindFlags = desc.bindFlags;
        stats.usage = static_cast<std::uint32_t>(desc.usage);
        stats.cpuAccessFlags = desc.cpuAccessFlags;
        break;
    }
    case REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        REX::W32::D3D11_TEXTURE3D_DESC desc{};
        static_cast<REX::W32::ID3D11Texture3D*>(resource)->GetDesc(std::addressof(desc));
        stats.width = desc.width;
        stats.height = desc.height;
        stats.depth = desc.depth;
        stats.format = static_cast<std::uint32_t>(desc.format);
        stats.bindFlags = desc.bindFlags;
        stats.usage = static_cast<std::uint32_t>(desc.usage);
        stats.cpuAccessFlags = desc.cpuAccessFlags;
        break;
    }
    default:
        break;
    }
    stats.described = true;
}

std::string ReplayMapResourceShape(const ReplayMapResourceStats& stats)
{
    switch (stats.dimension) {
    case REX::W32::D3D11_RESOURCE_DIMENSION_BUFFER:
        return std::format(
            "bytes={} bind=0x{:X} usage={} cpu=0x{:X}",
            stats.byteWidth,
            stats.bindFlags,
            stats.usage,
            stats.cpuAccessFlags);
    case REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE1D:
        return std::format(
            "{} fmt={} bind=0x{:X} usage={} cpu=0x{:X}",
            stats.width,
            stats.format,
            stats.bindFlags,
            stats.usage,
            stats.cpuAccessFlags);
    case REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE2D:
        return std::format(
            "{}x{} fmt={} bind=0x{:X} usage={} cpu=0x{:X}",
            stats.width,
            stats.height,
            stats.format,
            stats.bindFlags,
            stats.usage,
            stats.cpuAccessFlags);
    case REX::W32::D3D11_RESOURCE_DIMENSION_TEXTURE3D:
        return std::format(
            "{}x{}x{} fmt={} bind=0x{:X} usage={} cpu=0x{:X}",
            stats.width,
            stats.height,
            stats.depth,
            stats.format,
            stats.bindFlags,
            stats.usage,
            stats.cpuAccessFlags);
    default:
        return "unknown";
    }
}

std::uint32_t CurrentReplayMapDeferredKey()
{
    PhaseTelemetry::DeferredPrePassDetailKind kind{};
    std::uint32_t key = 0;
    if (!PhaseTelemetry::GetCurrentDeferredPrePassDetail(kind, key) ||
        kind != PhaseTelemetry::DeferredPrePassDetailKind::RenderCommandBufferPasses) {
        return UINT_MAX;
    }
    return key;
}

std::string DeferredCommandBufferKeyName(std::uint32_t key)
{
    if (key == UINT_MAX) {
        return "?";
    }

    const auto group = key >> 5;
    const auto subIdx = key & 0x1F;
    return std::format("{}.{}", group, subIdx);
}

std::string ReplayMapResourceKeySummary(const ReplayMapResourceStats& stats)
{
    if (stats.keyStats.empty()) {
        return "";
    }

    std::vector<std::pair<std::uint32_t, ReplayMapResourceStats::KeyStats>> rows;
    rows.reserve(stats.keyStats.size());
    for (const auto& entry : stats.keyStats) {
        rows.emplace_back(entry.first, entry.second);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second.ns > rhs.second.ns;
    });

    std::string out = "keys=";
    const std::size_t limit = (std::min<std::size_t>)(rows.size(), 4);
    for (std::size_t i = 0; i < limit; ++i) {
        if (i != 0) {
            out += ",";
        }
        out += std::format(
            "{}:{}/{}us dup={}/{}",
            DeferredCommandBufferKeyName(rows[i].first),
            rows[i].second.calls,
            rows[i].second.ns / 1000,
            rows[i].second.payloadDuplicateWrites,
            rows[i].second.payloadWrites);
    }
    return out;
}

std::uint64_t HashReplayPayload(const void* data, std::uint32_t size)
{
    constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;

    auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = kFnvOffset;
    for (std::uint32_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

bool IsTrackedReplayCBPayload(const ReplayMapResourceStats& stats, REX::W32::D3D11_MAP mapType)
{
    constexpr std::uint32_t kD3D11BindConstantBuffer = 0x4;
    return stats.dimension == REX::W32::D3D11_RESOURCE_DIMENSION_BUFFER &&
        stats.byteWidth == 128 &&
        (stats.bindFlags & kD3D11BindConstantBuffer) != 0 &&
        mapType == REX::W32::D3D11_MAP_WRITE_DISCARD;
}

bool ShouldLogReplayPayloadSample(std::uint32_t key, std::uint64_t hash)
{
    constexpr std::uint32_t kKey42 = (4u << 5) | 2u;
    constexpr std::uint32_t kKey43 = (4u << 5) | 3u;
    if (key != kKey42 && key != kKey43) {
        return false;
    }

    if (tl_loggedReplayPayloadSamples.size() >= 12) {
        return false;
    }

    const std::uint64_t sampleKey = (static_cast<std::uint64_t>(key) << 32) ^ hash;
    return tl_loggedReplayPayloadSamples.insert(sampleKey).second;
}

void LogReplayPayloadSample(const void* data, std::uint32_t key, std::uint64_t hash)
{
    std::array<float, 32> f{};
    std::memcpy(f.data(), data, sizeof(f));

    REX::INFO(
        "    CommandBufferD3D[CBPayload128]: key={} hash=0x{:016X} "
        "r0=({:.5g},{:.5g},{:.5g},{:.5g}) r1=({:.5g},{:.5g},{:.5g},{:.5g}) "
        "r2=({:.5g},{:.5g},{:.5g},{:.5g}) r3=({:.5g},{:.5g},{:.5g},{:.5g}) "
        "r4=({:.5g},{:.5g},{:.5g},{:.5g}) r5=({:.5g},{:.5g},{:.5g},{:.5g}) "
        "r6=({:.5g},{:.5g},{:.5g},{:.5g}) r7=({:.5g},{:.5g},{:.5g},{:.5g})",
        DeferredCommandBufferKeyName(key),
        hash,
        f[0], f[1], f[2], f[3],
        f[4], f[5], f[6], f[7],
        f[8], f[9], f[10], f[11],
        f[12], f[13], f[14], f[15],
        f[16], f[17], f[18], f[19],
        f[20], f[21], f[22], f[23],
        f[24], f[25], f[26], f[27],
        f[28], f[29], f[30], f[31]);
}

void MaybeLogReplayMapStats()
{
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - tl_replayMapStatsStart;
    if (elapsed < kReplayMapTelemetryInterval) {
        return;
    }

    const double secs = (std::max)(
        0.001,
        std::chrono::duration<double>(elapsed).count());
    std::vector<std::pair<REX::W32::ID3D11Resource*, ReplayMapResourceStats>> rows;
    rows.reserve(tl_replayMapStats.size());
    for (const auto& entry : tl_replayMapStats) {
        if (entry.second.mapCalls != 0 || entry.second.unmapCalls != 0) {
            rows.emplace_back(entry.first, entry.second);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
        return (lhs.second.mapNs + lhs.second.unmapNs) > (rhs.second.mapNs + rhs.second.unmapNs);
    });

    const std::size_t limit = std::min<std::size_t>(rows.size(), 10);
    for (std::size_t i = 0; i < limit; ++i) {
        const auto* resource = rows[i].first;
        const auto& stats = rows[i].second;
        const auto totalNs = stats.mapNs + stats.unmapNs;
        const auto totalCalls = stats.mapCalls + stats.unmapCalls;
        REX::INFO(
            "    CommandBufferD3D[MapResource:{}]: res={} type={} mapType={} flags=0x{:X} sub={} maps={} unmaps={} calls/s={:.1f} ms/s={:.2f} avgUs={:.2f} maxMapUs={:.1f} maxUnmapUs={:.1f} payloadDup={}/{} {} {}",
            i,
            static_cast<const void*>(resource),
            D3D11ResourceDimensionName(stats.dimension),
            D3D11MapTypeName(stats.mapType),
            stats.mapFlags,
            stats.subresource,
            stats.mapCalls,
            stats.unmapCalls,
            totalCalls / secs,
            (totalNs / 1'000'000.0) / secs,
            totalCalls ? (totalNs / 1000.0) / static_cast<double>(totalCalls) : 0.0,
            stats.maxMapNs / 1000.0,
            stats.maxUnmapNs / 1000.0,
            stats.payloadDuplicateWrites,
            stats.payloadWrites,
            ReplayMapResourceShape(stats),
            ReplayMapResourceKeySummary(stats));
    }

    tl_replayMapStats.clear();
    tl_loggedReplayPayloadSamples.clear();
    tl_replayMapStatsStart = now;
}

void NoteReplayMapResource(
    REX::W32::ID3D11Resource* resource,
    UINT subresource,
    REX::W32::D3D11_MAP mapType,
    UINT mapFlags,
    std::uint64_t ns)
{
    if (!PhaseTelemetry::IsInCommandBufferReplay() ||
        PhaseTelemetry::g_mode.load(std::memory_order_relaxed) != PhaseTelemetry::Mode::On ||
        !resource) {
        return;
    }

    auto& stats = tl_replayMapStats[resource];
    DescribeReplayMapResource(resource, stats);
    stats.mapType = mapType;
    stats.mapFlags = mapFlags;
    stats.subresource = subresource;
    ++stats.mapCalls;
    stats.mapNs += ns;
    stats.maxMapNs = (std::max)(stats.maxMapNs, ns);
    auto& keyStats = stats.keyStats[CurrentReplayMapDeferredKey()];
    ++keyStats.calls;
    keyStats.ns += ns;
    MaybeLogReplayMapStats();
}

void NoteReplayMappedPointer(
    REX::W32::ID3D11Resource* resource,
    REX::W32::D3D11_MAP mapType,
    REX::W32::D3D11_MAPPED_SUBRESOURCE* mappedResource)
{
    if (!PhaseTelemetry::IsInCommandBufferReplay() ||
        PhaseTelemetry::g_mode.load(std::memory_order_relaxed) != PhaseTelemetry::Mode::On ||
        !resource ||
        !mappedResource ||
        !mappedResource->data) {
        return;
    }

    auto statsIt = tl_replayMapStats.find(resource);
    if (statsIt == tl_replayMapStats.end() || !IsTrackedReplayCBPayload(statsIt->second, mapType)) {
        tl_activeReplayMaps.erase(resource);
        return;
    }

    tl_activeReplayMaps[resource] = ActiveReplayMapInfo{
        mappedResource->data,
        statsIt->second.byteWidth,
        statsIt->second.bindFlags,
        statsIt->second.dimension,
        mapType,
        CurrentReplayMapDeferredKey()
    };
}

void NoteReplayPayloadBeforeUnmap(REX::W32::ID3D11Resource* resource)
{
    if (!PhaseTelemetry::IsInCommandBufferReplay() ||
        PhaseTelemetry::g_mode.load(std::memory_order_relaxed) != PhaseTelemetry::Mode::On ||
        !resource) {
        return;
    }

    auto activeIt = tl_activeReplayMaps.find(resource);
    if (activeIt == tl_activeReplayMaps.end()) {
        return;
    }

    const auto active = activeIt->second;
    tl_activeReplayMaps.erase(activeIt);
    if (!active.data || active.byteWidth != 128) {
        return;
    }

    auto statsIt = tl_replayMapStats.find(resource);
    if (statsIt == tl_replayMapStats.end()) {
        return;
    }

    auto& stats = statsIt->second;
    const auto hash = HashReplayPayload(active.data, active.byteWidth);
    if (ShouldLogReplayPayloadSample(active.key, hash)) {
        LogReplayPayloadSample(active.data, active.key, hash);
    }
    const bool duplicate = stats.lastPayloadHashValid && stats.lastPayloadHash == hash;
    stats.lastPayloadHash = hash;
    stats.lastPayloadHashValid = true;
    ++stats.payloadWrites;
    if (duplicate) {
        ++stats.payloadDuplicateWrites;
    }

    auto& keyStats = stats.keyStats[active.key];
    ++keyStats.payloadWrites;
    if (duplicate) {
        ++keyStats.payloadDuplicateWrites;
    }
}

void NoteReplayUnmapResource(REX::W32::ID3D11Resource* resource, UINT subresource, std::uint64_t ns)
{
    if (!PhaseTelemetry::IsInCommandBufferReplay() ||
        PhaseTelemetry::g_mode.load(std::memory_order_relaxed) != PhaseTelemetry::Mode::On ||
        !resource) {
        return;
    }

    auto& stats = tl_replayMapStats[resource];
    DescribeReplayMapResource(resource, stats);
    stats.subresource = subresource;
    ++stats.unmapCalls;
    stats.unmapNs += ns;
    stats.maxUnmapNs = (std::max)(stats.maxUnmapNs, ns);
    auto& keyStats = stats.keyStats[CurrentReplayMapDeferredKey()];
    ++keyStats.calls;
    keyStats.ns += ns;
    MaybeLogReplayMapStats();
}

bool D3D11FilterReplayPSSetShaderResources(
    REX::W32::ID3D11DeviceContext* context,
    UINT startSlot,
    UINT numViews,
    REX::W32::ID3D11ShaderResourceView* const* shaderResourceViews)
{
    if (!PhaseTelemetry::IsInCommandBufferReplay() ||
        !COMMAND_BUFFER_REPLAY_DEDUPE_SRV ||
        startSlot >= kReplaySRVCacheSlots ||
        numViews > kReplaySRVCacheSlots ||
        startSlot + numViews > kReplaySRVCacheSlots ||
        (numViews != 0 && !shaderResourceViews)) {
        tl_replaySRVCallCache = {};
        return false;
    }

    UINT runStart = 0;
    UINT runCount = 0;

    auto flushRun = [&]() {
        if (runCount == 0) {
            return;
        }
        const UINT sourceOffset = runStart - startSlot;
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::ShaderResource,
            [&]() {
                D3D11Hooks::OriginalPSSetShaderResources(
                    context,
                    runStart,
                    runCount,
                    shaderResourceViews + sourceOffset);
            });
        runCount = 0;
    };

    for (UINT i = 0; i < numViews; ++i) {
        const UINT slot = startSlot + i;
        auto* view = shaderResourceViews[i];
        const bool changed =
            !tl_replaySRVCallCache.valid[slot] ||
            tl_replaySRVCallCache.views[slot] != view;

        tl_replaySRVCallCache.valid[slot] = true;
        tl_replaySRVCallCache.views[slot] = view;

        if (!changed) {
            PhaseTelemetry::OnCommandBufferD3DCall(
                PhaseTelemetry::CommandBufferD3DCallKind::ShaderResourceSkip,
                0);
            flushRun();
            continue;
        }

        if (runCount == 0) {
            runStart = slot;
            runCount = 1;
        } else if (runStart + runCount == slot) {
            ++runCount;
        } else {
            flushRun();
            runStart = slot;
            runCount = 1;
        }
    }
    flushRun();
    return true;
}

bool D3D11ShouldSkipReplayIASetInputLayout(REX::W32::ID3D11InputLayout* inputLayout)
{
    if (!COMMAND_BUFFER_REPLAY_DEDUPE_SRV || !PhaseTelemetry::IsInCommandBufferReplay()) {
        tl_replayIACache.inputLayoutValid = false;
        return false;
    }

    const bool duplicate =
        tl_replayIACache.inputLayoutValid &&
        tl_replayIACache.inputLayout == inputLayout;
    tl_replayIACache.inputLayoutValid = true;
    tl_replayIACache.inputLayout = inputLayout;
    return duplicate;
}

bool D3D11FilterReplayIASetVertexBuffers(
    REX::W32::ID3D11DeviceContext* context,
    UINT startSlot,
    UINT numBuffers,
    REX::W32::ID3D11Buffer* const* vertexBuffers,
    const UINT* strides,
    const UINT* offsets)
{
    if (!COMMAND_BUFFER_REPLAY_DEDUPE_SRV ||
        !PhaseTelemetry::IsInCommandBufferReplay() ||
        startSlot >= kReplayVertexBufferSlots ||
        numBuffers > kReplayVertexBufferSlots ||
        startSlot + numBuffers > kReplayVertexBufferSlots ||
        (numBuffers != 0 && (!vertexBuffers || !strides || !offsets))) {
        tl_replayIACache.vertexBufferValid = {};
        return false;
    }

    UINT runStart = 0;
    UINT runCount = 0;

    auto flushRun = [&]() {
        if (runCount == 0) {
            return;
        }
        const UINT sourceOffset = runStart - startSlot;
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::InputAssembly,
            [&]() {
                D3D11Hooks::OriginalIASetVertexBuffers(
                    context,
                    runStart,
                    runCount,
                    vertexBuffers + sourceOffset,
                    strides + sourceOffset,
                    offsets + sourceOffset);
            });
        runCount = 0;
    };

    for (UINT i = 0; i < numBuffers; ++i) {
        const UINT slot = startSlot + i;
        const ReplayVertexBufferState state{ vertexBuffers[i], strides[i], offsets[i] };
        const bool changed =
            !tl_replayIACache.vertexBufferValid[slot] ||
            tl_replayIACache.vertexBuffers[slot].buffer != state.buffer ||
            tl_replayIACache.vertexBuffers[slot].stride != state.stride ||
            tl_replayIACache.vertexBuffers[slot].offset != state.offset;

        tl_replayIACache.vertexBufferValid[slot] = true;
        tl_replayIACache.vertexBuffers[slot] = state;

        if (!changed) {
            PhaseTelemetry::OnCommandBufferD3DCall(
                PhaseTelemetry::CommandBufferD3DCallKind::InputAssemblySkip,
                0);
            flushRun();
            continue;
        }

        if (runCount == 0) {
            runStart = slot;
            runCount = 1;
        } else if (runStart + runCount == slot) {
            ++runCount;
        } else {
            flushRun();
            runStart = slot;
            runCount = 1;
        }
    }
    flushRun();
    return true;
}

bool D3D11ShouldSkipReplayIASetIndexBuffer(
    REX::W32::ID3D11Buffer* indexBuffer,
    REX::W32::DXGI_FORMAT format,
    UINT offset)
{
    if (!COMMAND_BUFFER_REPLAY_DEDUPE_SRV || !PhaseTelemetry::IsInCommandBufferReplay()) {
        tl_replayIACache.indexBufferValid = false;
        return false;
    }

    const bool duplicate =
        tl_replayIACache.indexBufferValid &&
        tl_replayIACache.indexBuffer == indexBuffer &&
        tl_replayIACache.indexFormat == format &&
        tl_replayIACache.indexOffset == offset;
    tl_replayIACache.indexBufferValid = true;
    tl_replayIACache.indexBuffer = indexBuffer;
    tl_replayIACache.indexFormat = format;
    tl_replayIACache.indexOffset = offset;
    return duplicate;
}

D3D11PSSetShaderResult D3D11OnPSSetShaderBefore_Internal(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11PixelShader* pixelShader)
{
    D3D11PSSetShaderResult result{};
    result.shader = pixelShader;
    if (g_customPassRendering) {
        return result;
    }

    g_currentOriginalPixelShader.store(pixelShader, std::memory_order_release);
    if (SHADERENGINE_EFFECTS_ON) {
        ArmCustomPassDrawBatch(pixelShader);
    } else {
        ArmCustomPassDrawBatch(nullptr);
    }

    // Light-tracker capture at PS-bind time. Engine sets RTVs / blend /
    // scissor / cb2 / SRVs before PSSetShader, so all the per-pass state
    // we want is live at this moment. Cheap no-op when not capturing.
    LightTracker::OnPSBind(context, pixelShader);
    if (!SHADERENGINE_EFFECTS_ON) {
        return result;
    }
    // Trigger any customPass blocks attached to this original shader. The
    // pass runs immediately before the hook forwards to the original
    // PSSetShader so the engine state we save/restore is the state the engine
    // just established for the upcoming shader.
    if (pixelShader && !g_isCreatingReplacementShader && !g_bindingInjectedPixelResources) {
        result.customPassFired = CustomPass::g_registry.OnBeforeShaderBound(context, pixelShader);
    }
    if (pixelShader) {
        // Check if this shader is matched with a replacement shader in our DB
        if (g_ShaderDB.IsEntryMatched(pixelShader)) {
            g_ShaderDB.SetEntryRecentlyUsed(pixelShader, true);
            auto* matchedDefinition = g_ShaderDB.GetMatchedDefinition(pixelShader);
            if (matchedDefinition &&
                matchedDefinition->shadowUpgrade &&
                !SHADOW_UPGRADE_ON) {
                return result;
            }
            // If the HLSL watcher flagged a disk change for this definition,
            // drop the cached compiled shader + replacement pointers BEFORE we
            // read them below. Done here on the render thread so the D3D11
            // Release happens on the immediate-context-owning thread.
            MaybeApplyHlslHotReload_Internal(matchedDefinition);
            auto* replacementPixelShader = g_ShaderDB.GetReplacementShader(pixelShader);
            if (replacementPixelShader) {
                if (DEBUGGING) {
                    REX::INFO(
                        "MyPSSetShader: Replacing pixel shader with matched "
                        "replacement for definition '{}'",
                        matchedDefinition ? matchedDefinition->id : "Unknown");
                }
                result.shader = replacementPixelShader;
                result.usingReplacementPixelShader = true;
                result.activeReplacementDef = matchedDefinition;
            } else {
                if (matchedDefinition && !matchedDefinition->buggy) {
                    if (DEBUGGING) {
                        REX::INFO("MyPSSetShader: Shader is matched but no replacement shader found, trying to compile...");
                    }
                    if (CompileShader_Internal(matchedDefinition)) {
                        g_ShaderDB.SetReplacementShader(pixelShader, matchedDefinition->loadedPixelShader);
                        if (DEBUGGING) {
                            REX::INFO("MyPSSetShader: Compiled replacement shader for definition '{}'", matchedDefinition->id);
                            REX::INFO(
                                "MyPSSetShader: Replacing pixel shader with "
                                "newly compiled replacement for definition '{}'",
                                matchedDefinition->id);
                        }
                        result.shader = g_ShaderDB.GetReplacementShader(pixelShader);
                        result.usingReplacementPixelShader = result.shader != nullptr;
                        result.activeReplacementDef =
                            result.usingReplacementPixelShader ?
                                matchedDefinition :
                                nullptr;
                    } else {
                        REX::WARN("MyPSSetShader: Failed to compile replacement shader for definition '{}'", matchedDefinition->id);
                        matchedDefinition->buggy = true;
                    }
                }
            }
        }
    }

    return result;
}

void D3D11OnPSSetShaderAfter_Internal(
    REX::W32::ID3D11DeviceContext* context,
    const D3D11PSSetShaderResult& result)
{
    if (g_customPassRendering) {
        return;
    }
    if (!SHADERENGINE_EFFECTS_ON) {
        ShaderResources::SetActiveReplacementPixelShaderUsage(nullptr, false);
        return;
    }
    ShaderResources::SetActiveReplacementPixelShaderUsage(result.activeReplacementDef, result.usingReplacementPixelShader);
    if (ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind()) {
        ShaderResources::BindInjectedPixelShaderResources(context);
    }
    // If a customPass fired, the engine's next draw still expects the injected
    // resource set we publish for replacement shaders (depth, GFXInjected, etc.)
    // to be present on its slots. The pass already re-binds them, but there is
    // no harm in publishing them again here when running a replacement.
    if (result.customPassFired && ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind()) {
        ShaderResources::BindInjectedPixelShaderResources(context);
    }
}

REX::W32::ID3D11VertexShader* D3D11OnVSSetShader_Internal(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11VertexShader* vertexShader)
{
    if (g_customPassRendering) {
        return vertexShader;
    }
    if (!SHADERENGINE_EFFECTS_ON) {
        return vertexShader;
    }
    if (vertexShader) {
        // Check if this shader is matched with a replacement shader in our DB
        if (g_ShaderDB.IsEntryMatched(vertexShader)) {
            g_ShaderDB.SetEntryRecentlyUsed(vertexShader, true);
            auto* matchedDefinition = g_ShaderDB.GetMatchedDefinition(vertexShader);
            // See PSSetShader for rationale: we evict cached compiled state
            // here, before reading the replacement, so the D3D11 Release runs
            // on the render thread.
            MaybeApplyHlslHotReload_Internal(matchedDefinition);
            auto* replacementVertexShader = g_ShaderDB.GetReplacementShader(vertexShader);
            if (replacementVertexShader) {
                if (DEBUGGING) {
                    REX::INFO("MyVSSetShader: Replacing vertex shader with matched replacement for definition '{}'", matchedDefinition ? matchedDefinition->id : "Unknown");
                }
                vertexShader = replacementVertexShader;
                ShaderResources::BindInjectedVertexShaderResources(context);
                ShaderResources::BindReplacementSRVResources(context, matchedDefinition, /*pixelStage=*/false);
                ShaderResources::BindReplacementTextureResources(context, matchedDefinition, /*pixelStage=*/false);
            } else {
                if (matchedDefinition && !matchedDefinition->buggy) {
                    if (DEBUGGING) {
                        REX::INFO("MyVSSetShader: Shader is matched but no replacement shader found, trying to compile...");
                    }
                    if (CompileShader_Internal(matchedDefinition)) {
                        g_ShaderDB.SetReplacementShader(vertexShader, matchedDefinition->loadedVertexShader);
                        if (DEBUGGING) {
                            REX::INFO("MyVSSetShader: Compiled replacement shader for definition '{}'", matchedDefinition->id);
                            REX::INFO("MyVSSetShader: Replacing vertex shader with newly compiled replacement for definition '{}'", matchedDefinition->id);
                        }
                        vertexShader = g_ShaderDB.GetReplacementShader(vertexShader);
                        ShaderResources::BindInjectedVertexShaderResources(context);
                        ShaderResources::BindReplacementSRVResources(context, matchedDefinition, /*pixelStage=*/false);
                        ShaderResources::BindReplacementTextureResources(context, matchedDefinition, /*pixelStage=*/false);
                    } else {
                        REX::WARN("MyVSSetShader: Failed to compile replacement shader for definition '{}'", matchedDefinition->id);
                        matchedDefinition->buggy = true;
                    }
                }
            }
        }
    }

    return vertexShader;
}

void D3D11OnCreatePixelShader_Internal(
    HRESULT hr,
    const void* shaderBytecode,
    SIZE_T bytecodeLength,
    REX::W32::ID3D11PixelShader** pixelShader)
{
    if (REX::W32::SUCCESS(hr) && pixelShader && *pixelShader) {
        std::vector<uint8_t> bytecode(bytecodeLength);
        std::memcpy(bytecode.data(), shaderBytecode, bytecodeLength);
        if (g_ShaderDB.HasEntry(*pixelShader)) {
            return;
        }
        ShaderDBEntry entry = AnalyzeShader_Internal(*pixelShader, nullptr, std::move(bytecode), bytecodeLength);
        g_ShaderDB.AddShaderEntry(std::move(entry));
    }
}

void D3D11OnCreateVertexShader_Internal(
    HRESULT hr,
    const void* shaderBytecode,
    SIZE_T bytecodeLength,
    REX::W32::ID3D11VertexShader** vertexShader)
{
    if (REX::W32::SUCCESS(hr) && vertexShader && *vertexShader) {
        std::vector<uint8_t> bytecode(bytecodeLength);
        std::memcpy(bytecode.data(), shaderBytecode, bytecodeLength);
        if (g_ShaderDB.HasEntry(*vertexShader)) {
            return;
        }
        ShaderDBEntry entry = AnalyzeShader_Internal(nullptr, *vertexShader, std::move(bytecode), bytecodeLength);
        g_ShaderDB.AddShaderEntry(std::move(entry));
    }
}
namespace
{
    using BSD3DResourceCreatorFlush_t = void (*)(void* resourceCreator, REX::W32::ID3D11DeviceContext* context);
    using BSD3DResourceCreatorEscalatePriorityForResource_t = void (*)(void* resourceCreator, const void* resource, std::uint32_t escalationResourceId);
    using WaitForSingleObjectEx_t = DWORD(WINAPI*)(HANDLE handle, DWORD milliseconds, BOOL alertable);

    REL::Relocation<std::uintptr_t> ptr_BSD3DResourceCreatorFlush{ REL::ID{ 1210092, 2277436 } };
    REL::Relocation<std::uintptr_t> ptr_BSD3DResourceCreatorEscalatePriorityForResource{ REL::ID{ 1136324, 2277416 } };

    BSD3DResourceCreatorFlush_t OriginalBSD3DResourceCreatorFlush = nullptr;
    BSD3DResourceCreatorEscalatePriorityForResource_t OriginalBSD3DResourceCreatorEscalatePriorityForResource = nullptr;
    WaitForSingleObjectEx_t OriginalProcessCommandBufferWaitForSingleObjectEx = nullptr;

    void HookedBSD3DResourceCreatorFlush(void* resourceCreator, REX::W32::ID3D11DeviceContext* context)
    {
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::ResourceFlush,
            [&]() {
                OriginalBSD3DResourceCreatorFlush(resourceCreator, context);
            });
    }

    void HookedBSD3DResourceCreatorEscalatePriorityForResource(
        void* resourceCreator,
        const void* resource,
        std::uint32_t escalationResourceId)
    {
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::ResourceEscalate,
            [&]() {
                OriginalBSD3DResourceCreatorEscalatePriorityForResource(
                    resourceCreator,
                    resource,
                    escalationResourceId);
            });
    }

    DWORD WINAPI HookedProcessCommandBufferWaitForSingleObjectEx(
        HANDLE handle,
        DWORD milliseconds,
        BOOL alertable)
    {
        return ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::ResourceWait,
            [&]() {
                return OriginalProcessCommandBufferWaitForSingleObjectEx(handle, milliseconds, alertable);
            });
    }

    bool PatchMainModuleImport(const char* dllName, const char* importName, void* hook, void** original)
    {
        auto* module = reinterpret_cast<std::uint8_t*>(::GetModuleHandleW(nullptr));
        if (!module) {
            return false;
        }

        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }

        const auto& importDir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!importDir.VirtualAddress || !importDir.Size) {
            return false;
        }

        auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module + importDir.VirtualAddress);
        for (; desc->Name; ++desc) {
            const char* currentDll = reinterpret_cast<const char*>(module + desc->Name);
            if (_stricmp(currentDll, dllName) != 0) {
                continue;
            }

            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(module + desc->FirstThunk);
            auto* nameThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                module + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
            for (; nameThunk->u1.AddressOfData; ++nameThunk, ++thunk) {
                if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) {
                    continue;
                }

                auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(module + nameThunk->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), importName) != 0) {
                    continue;
                }

                auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
                if (*slot == hook) {
                    return true;
                }

                DWORD oldProtect = 0;
                if (!::VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    return false;
                }

                if (original && !*original) {
                    *original = *slot;
                }
                *slot = hook;

                DWORD ignored = 0;
                ::VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
                ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
                return true;
            }
        }

        return false;
    }

    void InstallCommandBufferResourceTelemetryHooks()
    {
        static bool attempted = false;
        if (attempted) {
            return;
        }
        attempted = true;

        if (!OriginalBSD3DResourceCreatorFlush) {
            constexpr std::size_t kFlushPrologueSize = 10;
            OriginalBSD3DResourceCreatorFlush =
                Hooks::CreateBranchGateway5<BSD3DResourceCreatorFlush_t>(
                    ptr_BSD3DResourceCreatorFlush,
                    kFlushPrologueSize,
                    reinterpret_cast<void*>(&HookedBSD3DResourceCreatorFlush));
            if (!OriginalBSD3DResourceCreatorFlush) {
                REX::WARN("InstallCommandBufferResourceTelemetryHooks: failed to install BSD3DResourceCreator::Flush hook");
            } else {
                REX::INFO("InstallCommandBufferResourceTelemetryHooks: BSD3DResourceCreator::Flush hook installed");
            }
        }

        if (!OriginalBSD3DResourceCreatorEscalatePriorityForResource) {
            constexpr std::size_t kEscalatePrologueSize = 15;
            OriginalBSD3DResourceCreatorEscalatePriorityForResource =
                Hooks::CreateBranchGateway5<BSD3DResourceCreatorEscalatePriorityForResource_t>(
                    ptr_BSD3DResourceCreatorEscalatePriorityForResource,
                    kEscalatePrologueSize,
                    reinterpret_cast<void*>(&HookedBSD3DResourceCreatorEscalatePriorityForResource));
            if (!OriginalBSD3DResourceCreatorEscalatePriorityForResource) {
                REX::WARN("InstallCommandBufferResourceTelemetryHooks: failed to install BSD3DResourceCreator::EscalatePriorityForResource hook");
            } else {
                REX::INFO("InstallCommandBufferResourceTelemetryHooks: BSD3DResourceCreator::EscalatePriorityForResource hook installed");
            }
        }

        if (OriginalProcessCommandBufferWaitForSingleObjectEx) {
            return;
        }

        void* originalWait = nullptr;
        if (PatchMainModuleImport(
                "KERNEL32.dll",
                "WaitForSingleObjectEx",
                reinterpret_cast<void*>(&HookedProcessCommandBufferWaitForSingleObjectEx),
                &originalWait)) {
            OriginalProcessCommandBufferWaitForSingleObjectEx =
                reinterpret_cast<WaitForSingleObjectEx_t>(originalWait);
            REX::INFO("InstallCommandBufferResourceTelemetryHooks: WaitForSingleObjectEx import hook installed");
        } else {
            REX::WARN("InstallCommandBufferResourceTelemetryHooks: failed to install WaitForSingleObjectEx import hook");
        }
    }

    HRESULT STDMETHODCALLTYPE MyPresent(
        REX::W32::IDXGISwapChain* swapChain,
        UINT syncInterval,
        UINT flags)
    {
        D3D11Hooks::EnsureDrawHooksPresent();
        D3D11OnPresent_Internal();
        return D3D11Hooks::OriginalPresent(swapChain, syncInterval, flags);
    }

    void STDMETHODCALLTYPE MyPSSetShaderResources(
        REX::W32::ID3D11DeviceContext* context,
        UINT startSlot,
        UINT numViews,
        REX::W32::ID3D11ShaderResourceView* const* shaderResourceViews)
    {
        if (D3D11FilterReplayPSSetShaderResources(context, startSlot, numViews, shaderResourceViews)) {
            return;
        }

        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::ShaderResource,
            [&]() {
                D3D11Hooks::OriginalPSSetShaderResources(context, startSlot, numViews, shaderResourceViews);
            });
        D3D11OnPSSetShaderResources_Internal(context);
    }

    void STDMETHODCALLTYPE MyOMSetRenderTargets(
        REX::W32::ID3D11DeviceContext* context,
        UINT numViews,
        REX::W32::ID3D11RenderTargetView* const* renderTargetViews,
        REX::W32::ID3D11DepthStencilView* depthStencilView)
    {
        D3D11Hooks::OriginalOMSetRenderTargets(context, numViews, renderTargetViews, depthStencilView);
        D3D11OnOMSetRenderTargets_Internal(context, numViews, renderTargetViews, depthStencilView);
    }

    void STDMETHODCALLTYPE MyClearDepthStencilView(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11DepthStencilView* depthStencilView,
        UINT clearFlags,
        FLOAT depth,
        UINT8 stencil)
    {
        if (D3D11ShouldSuppressClearDepthStencilView_Internal(context, depthStencilView, clearFlags, depth, stencil)) {
            return;
        }

        D3D11Hooks::OriginalClearDepthStencilView(context, depthStencilView, clearFlags, depth, stencil);
    }

    void STDMETHODCALLTYPE MyDrawIndexed(
        REX::W32::ID3D11DeviceContext* context,
        UINT indexCount,
        UINT startIndexLocation,
        INT baseVertexLocation)
    {
        D3D11OnDraw_Internal(context, "d3d11-DrawIndexed");
        const DrawIndexedPayload payload{
            indexCount, startIndexLocation, baseVertexLocation
        };
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::Draw,
            [&]() {
                if (!ShadowMapDeferredLighting::TryRenderLiveDraw(
                        context, ReplayDrawIndexed, &payload) &&
                    !TiledDeferredLighting::TryRenderLiveComposite(
                        context, ReplayDrawIndexed, &payload)) {
                    ReplayDrawIndexed(context, &payload);
                }
            });
    }

    void STDMETHODCALLTYPE MyDraw(
        REX::W32::ID3D11DeviceContext* context,
        UINT vertexCount,
        UINT startVertexLocation)
    {
        D3D11OnDraw_Internal(context, "d3d11-Draw");
        const DrawPayload payload{ vertexCount, startVertexLocation };
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::Draw,
            [&]() {
                if (!ShadowMapDeferredLighting::TryRenderLiveDraw(
                        context, ReplayDraw, &payload) &&
                    !TiledDeferredLighting::TryRenderLiveComposite(
                        context, ReplayDraw, &payload)) {
                    ReplayDraw(context, &payload);
                }
            });
    }

    void STDMETHODCALLTYPE MyDrawIndexedInstanced(
        REX::W32::ID3D11DeviceContext* context,
        UINT indexCountPerInstance,
        UINT instanceCount,
        UINT startIndexLocation,
        INT baseVertexLocation,
        UINT startInstanceLocation)
    {
        D3D11OnDraw_Internal(context, "d3d11-DrawIndexedInstanced");
        const DrawIndexedInstancedPayload payload{
            indexCountPerInstance,
            instanceCount,
            startIndexLocation,
            baseVertexLocation,
            startInstanceLocation
        };
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::Draw,
            [&]() {
                if (!ShadowMapDeferredLighting::TryRenderLiveDraw(
                        context, ReplayDrawIndexedInstanced, &payload) &&
                    !TiledDeferredLighting::TryRenderLiveComposite(
                        context, ReplayDrawIndexedInstanced, &payload)) {
                    ReplayDrawIndexedInstanced(context, &payload);
                }
            });
    }

    void STDMETHODCALLTYPE MyDrawInstanced(
        REX::W32::ID3D11DeviceContext* context,
        UINT vertexCountPerInstance,
        UINT instanceCount,
        UINT startVertexLocation,
        UINT startInstanceLocation)
    {
        D3D11OnDraw_Internal(context, "d3d11-DrawInstanced");
        const DrawInstancedPayload payload{
            vertexCountPerInstance,
            instanceCount,
            startVertexLocation,
            startInstanceLocation
        };
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::Draw,
            [&]() {
                if (!ShadowMapDeferredLighting::TryRenderLiveDraw(
                        context, ReplayDrawInstanced, &payload) &&
                    !TiledDeferredLighting::TryRenderLiveComposite(
                        context, ReplayDrawInstanced, &payload)) {
                    ReplayDrawInstanced(context, &payload);
                }
            });
    }

    HRESULT STDMETHODCALLTYPE MyMap(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11Resource* resource,
        UINT subresource,
        REX::W32::D3D11_MAP mapType,
        UINT mapFlags,
        REX::W32::D3D11_MAPPED_SUBRESOURCE* mappedResource)
    {
#if !SHADERENGINE_ENABLE_PHASE_TELEMETRY
        return D3D11Hooks::OriginalMap(context, resource, subresource, mapType, mapFlags, mappedResource);
#else
        if (!PhaseTelemetry::IsInCommandBufferReplay()) {
            return D3D11Hooks::OriginalMap(context, resource, subresource, mapType, mapFlags, mappedResource);
        }

        const auto t0 = std::chrono::steady_clock::now();
        const HRESULT result =
            D3D11Hooks::OriginalMap(context, resource, subresource, mapType, mapFlags, mappedResource);
        const auto t1 = std::chrono::steady_clock::now();
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        PhaseTelemetry::OnCommandBufferD3DCall(PhaseTelemetry::CommandBufferD3DCallKind::Map, ns);
        NoteReplayMapResource(resource, subresource, mapType, mapFlags, ns);
        if (REX::W32::SUCCESS(result)) {
            NoteReplayMappedPointer(resource, mapType, mappedResource);
        }
        return result;
#endif
    }

    void STDMETHODCALLTYPE MyUnmap(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11Resource* resource,
        UINT subresource)
    {
#if !SHADERENGINE_ENABLE_PHASE_TELEMETRY
        D3D11Hooks::OriginalUnmap(context, resource, subresource);
#else
        if (!PhaseTelemetry::IsInCommandBufferReplay()) {
            D3D11Hooks::OriginalUnmap(context, resource, subresource);
            return;
        }

        NoteReplayPayloadBeforeUnmap(resource);
        const auto t0 = std::chrono::steady_clock::now();
        D3D11Hooks::OriginalUnmap(context, resource, subresource);
        const auto t1 = std::chrono::steady_clock::now();
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        PhaseTelemetry::OnCommandBufferD3DCall(PhaseTelemetry::CommandBufferD3DCallKind::Unmap, ns);
        NoteReplayUnmapResource(resource, subresource, ns);
#endif
    }

    void STDMETHODCALLTYPE MyVSSetConstantBuffers(
        REX::W32::ID3D11DeviceContext* context,
        UINT startSlot,
        UINT numBuffers,
        REX::W32::ID3D11Buffer* const* constantBuffers)
    {
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::ConstantBuffer,
            [&]() {
                D3D11Hooks::OriginalVSSetConstantBuffers(context, startSlot, numBuffers, constantBuffers);
            });
    }

    void STDMETHODCALLTYPE MyPSSetConstantBuffers(
        REX::W32::ID3D11DeviceContext* context,
        UINT startSlot,
        UINT numBuffers,
        REX::W32::ID3D11Buffer* const* constantBuffers)
    {
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::ConstantBuffer,
            [&]() {
                D3D11Hooks::OriginalPSSetConstantBuffers(context, startSlot, numBuffers, constantBuffers);
            });
    }

    void STDMETHODCALLTYPE MyPSSetSamplers(
        REX::W32::ID3D11DeviceContext* context,
        UINT startSlot,
        UINT numSamplers,
        REX::W32::ID3D11SamplerState* const* samplers)
    {
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::Sampler,
            [&]() {
                D3D11Hooks::OriginalPSSetSamplers(context, startSlot, numSamplers, samplers);
            });
    }

    void STDMETHODCALLTYPE MyIASetInputLayout(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11InputLayout* inputLayout)
    {
        if (D3D11ShouldSkipReplayIASetInputLayout(inputLayout)) {
            PhaseTelemetry::OnCommandBufferD3DCall(
                PhaseTelemetry::CommandBufferD3DCallKind::InputAssemblySkip,
                0);
            return;
        }

        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::InputAssembly,
            [&]() {
                D3D11Hooks::OriginalIASetInputLayout(context, inputLayout);
            });
    }

    void STDMETHODCALLTYPE MyIASetVertexBuffers(
        REX::W32::ID3D11DeviceContext* context,
        UINT startSlot,
        UINT numBuffers,
        REX::W32::ID3D11Buffer* const* vertexBuffers,
        const UINT* strides,
        const UINT* offsets)
    {
        if (D3D11FilterReplayIASetVertexBuffers(context, startSlot, numBuffers, vertexBuffers, strides, offsets)) {
            return;
        }

        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::InputAssembly,
            [&]() {
                D3D11Hooks::OriginalIASetVertexBuffers(context, startSlot, numBuffers, vertexBuffers, strides, offsets);
            });
    }

    void STDMETHODCALLTYPE MyIASetIndexBuffer(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11Buffer* indexBuffer,
        REX::W32::DXGI_FORMAT format,
        UINT offset)
    {
        if (D3D11ShouldSkipReplayIASetIndexBuffer(indexBuffer, format, offset)) {
            PhaseTelemetry::OnCommandBufferD3DCall(
                PhaseTelemetry::CommandBufferD3DCallKind::InputAssemblySkip,
                0);
            return;
        }

        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::InputAssembly,
            [&]() {
                D3D11Hooks::OriginalIASetIndexBuffer(context, indexBuffer, format, offset);
            });
    }

    void STDMETHODCALLTYPE MyOMSetBlendState(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11BlendState* blendState,
        const FLOAT blendFactor[4],
        UINT sampleMask)
    {
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::State,
            [&]() {
                D3D11Hooks::OriginalOMSetBlendState(context, blendState, blendFactor, sampleMask);
            });
    }

    void STDMETHODCALLTYPE MyOMSetDepthStencilState(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11DepthStencilState* depthStencilState,
        UINT stencilRef)
    {
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::State,
            [&]() {
                D3D11Hooks::OriginalOMSetDepthStencilState(context, depthStencilState, stencilRef);
            });
    }

    void STDMETHODCALLTYPE MyRSSetState(
        REX::W32::ID3D11DeviceContext* context,
        void* rasterizerState)
    {
        ProfileCommandBufferD3DCall(
            PhaseTelemetry::CommandBufferD3DCallKind::State,
            [&]() {
                D3D11Hooks::OriginalRSSetState(context, rasterizerState);
            });
    }

    void STDMETHODCALLTYPE MyPSSetShader(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11PixelShader* pixelShader,
        REX::W32::ID3D11ClassInstance* const* classInstances,
        UINT numClassInstances)
    {
        ResetReplayStateCache();
        auto result = D3D11OnPSSetShaderBefore_Internal(context, pixelShader);
        D3D11Hooks::OriginalPSSetShader(context, result.shader, classInstances, numClassInstances);
        D3D11OnPSSetShaderAfter_Internal(context, result);
    }

    void STDMETHODCALLTYPE MyVSSetShader(
        REX::W32::ID3D11DeviceContext* context,
        REX::W32::ID3D11VertexShader* vertexShader,
        REX::W32::ID3D11ClassInstance* const* classInstances,
        UINT numClassInstances)
    {
        vertexShader = D3D11OnVSSetShader_Internal(context, vertexShader);
        D3D11Hooks::OriginalVSSetShader(context, vertexShader, classInstances, numClassInstances);
    }

    HRESULT STDMETHODCALLTYPE MyCreatePixelShader(
        REX::W32::ID3D11Device* device,
        const void* shaderBytecode,
        SIZE_T bytecodeLength,
        REX::W32::ID3D11ClassLinkage* classLinkage,
        REX::W32::ID3D11PixelShader** pixelShader)
    {
        if (g_isCreatingReplacementShader) {
            return D3D11Hooks::OriginalCreatePixelShader(device, shaderBytecode, bytecodeLength, classLinkage, pixelShader);
        }

        const HRESULT hr = D3D11Hooks::OriginalCreatePixelShader(device, shaderBytecode, bytecodeLength, classLinkage, pixelShader);
        D3D11OnCreatePixelShader_Internal(hr, shaderBytecode, bytecodeLength, pixelShader);
        return hr;
    }

    HRESULT STDMETHODCALLTYPE MyCreateVertexShader(
        REX::W32::ID3D11Device* device,
        const void* shaderBytecode,
        SIZE_T bytecodeLength,
        REX::W32::ID3D11ClassLinkage* classLinkage,
        REX::W32::ID3D11VertexShader** vertexShader)
    {
        if (g_isCreatingReplacementShader) {
            return D3D11Hooks::OriginalCreateVertexShader(device, shaderBytecode, bytecodeLength, classLinkage, vertexShader);
        }

        const HRESULT hr = D3D11Hooks::OriginalCreateVertexShader(device, shaderBytecode, bytecodeLength, classLinkage, vertexShader);
        D3D11OnCreateVertexShader_Internal(hr, shaderBytecode, bytecodeLength, vertexShader);
        return hr;
    }

    BOOL WINAPI HookedClipCursor(const RECT* rect)
    {
        if (g_imguiInitialized && UIIsMenuOpen()) {
            rect = UIIsWindowActive() ? UIGetWindowRect() : nullptr;
        }
        return D3D11Hooks::OriginalClipCursor(rect);
    }

    HRESULT WINAPI HookedD3D11CreateDeviceAndSwapChain(
        IDXGIAdapter* adapter,
        D3D_DRIVER_TYPE driverType,
        HMODULE software,
        UINT flags,
        const D3D_FEATURE_LEVEL* featureLevels,
        UINT featureLevelCount,
        UINT sdkVersion,
        const DXGI_SWAP_CHAIN_DESC* swapChainDesc,
        IDXGISwapChain** swapChain,
        ID3D11Device** device,
        D3D_FEATURE_LEVEL* featureLevel,
        ID3D11DeviceContext** immediateContext)
    {
        const HRESULT hr = D3D11Hooks::OriginalD3D11CreateDeviceAndSwapChain(
            adapter,
            driverType,
            software,
            flags,
            featureLevels,
            featureLevelCount,
            sdkVersion,
            swapChainDesc,
            swapChain,
            device,
            featureLevel,
            immediateContext);

        if (swapChainDesc) {
            g_outputWindow = swapChainDesc->OutputWindow;
        }

        if (!g_rendererData || !g_rendererData->device) {
            g_rendererData = RE::BSGraphics::GetRendererData();
        }
        if (!g_rendererData || !g_rendererData->device) {
            return hr;
        }

        auto* deviceVTable = *reinterpret_cast<void***>(g_rendererData->device);
        if (!Hooks::InstallVTableSlot(deviceVTable, 15, reinterpret_cast<void*>(MyCreatePixelShader), D3D11Hooks::OriginalCreatePixelShader)) {
            REX::WARN("InstallShaderCreationHooks_Internal: VirtualProtect failed for CreatePixelShader");
            return hr;
        }
        REX::INFO("InstallShaderCreationHooks_Internal: CreatePixelShader hook installed");

        if (!Hooks::InstallVTableSlot(deviceVTable, 12, reinterpret_cast<void*>(MyCreateVertexShader), D3D11Hooks::OriginalCreateVertexShader)) {
            REX::WARN("InstallShaderCreationHooks_Internal: VirtualProtect failed for CreateVertexShader");
            return hr;
        }
        REX::INFO("InstallShaderCreationHooks_Internal: CreateVertexShader hook installed");

        return hr;
    }
}

namespace D3D11Hooks
{
    void ResetCommandBufferReplayState()
    {
        ResetReplayStateCache();
    }

    void EnsureDrawHooksPresent()
    {
        if (!g_rendererData) {
            g_rendererData = RE::BSGraphics::GetRendererData();
        }
        if (!g_rendererData || !g_rendererData->context) {
            return;
        }

        auto* vtable = *reinterpret_cast<void***>(g_rendererData->context);
        if (!vtable) {
            return;
        }

        Hooks::EnsureVTableSlot(vtable, 12, reinterpret_cast<void*>(MyDrawIndexed), OriginalDrawIndexed);
        Hooks::EnsureVTableSlot(vtable, 13, reinterpret_cast<void*>(MyDraw), OriginalDraw);
        Hooks::EnsureVTableSlot(vtable, 14, reinterpret_cast<void*>(MyMap), OriginalMap);
        Hooks::EnsureVTableSlot(vtable, 15, reinterpret_cast<void*>(MyUnmap), OriginalUnmap);
        Hooks::EnsureVTableSlot(vtable, 16, reinterpret_cast<void*>(MyPSSetConstantBuffers), OriginalPSSetConstantBuffers);
        Hooks::EnsureVTableSlot(vtable, 17, reinterpret_cast<void*>(MyIASetInputLayout), OriginalIASetInputLayout);
        Hooks::EnsureVTableSlot(vtable, 18, reinterpret_cast<void*>(MyIASetVertexBuffers), OriginalIASetVertexBuffers);
        Hooks::EnsureVTableSlot(vtable, 19, reinterpret_cast<void*>(MyIASetIndexBuffer), OriginalIASetIndexBuffer);
        Hooks::EnsureVTableSlot(vtable, 20, reinterpret_cast<void*>(MyDrawIndexedInstanced), OriginalDrawIndexedInstanced);
        Hooks::EnsureVTableSlot(vtable, 21, reinterpret_cast<void*>(MyDrawInstanced), OriginalDrawInstanced);
        Hooks::EnsureVTableSlot(vtable, 53, reinterpret_cast<void*>(MyClearDepthStencilView), OriginalClearDepthStencilView);
        Hooks::EnsureVTableSlot(vtable, 7, reinterpret_cast<void*>(MyVSSetConstantBuffers), OriginalVSSetConstantBuffers);
        Hooks::EnsureVTableSlot(vtable, 8, reinterpret_cast<void*>(MyPSSetShaderResources), OriginalPSSetShaderResources);
        Hooks::EnsureVTableSlot(vtable, 10, reinterpret_cast<void*>(MyPSSetSamplers), OriginalPSSetSamplers);
        Hooks::EnsureVTableSlot(vtable, 35, reinterpret_cast<void*>(MyOMSetBlendState), OriginalOMSetBlendState);
        Hooks::EnsureVTableSlot(vtable, 36, reinterpret_cast<void*>(MyOMSetDepthStencilState), OriginalOMSetDepthStencilState);
        Hooks::EnsureVTableSlot(vtable, 43, reinterpret_cast<void*>(MyRSSetState), OriginalRSSetState);
#if SHADERENGINE_ENABLE_PHASE_TELEMETRY
        InstallCommandBufferResourceTelemetryHooks();
#endif
    }
}

bool InstallGFXHooks_Internal()
{
    if (!g_rendererData) {
        g_rendererData = RE::BSGraphics::GetRendererData();
    }
    if (!g_rendererData || !g_rendererData->device) {
        REX::WARN("InstallGFXHooks_Internal: Cannot install hook: device not ready");
        return false;
    }

    auto installVTableHook = [](void** vtable, std::size_t index, void* hook, auto& original, const char* name) {
        if (!Hooks::InstallVTableSlot(vtable, index, hook, original)) {
            REX::WARN("InstallGFXHooks_Internal: VirtualProtect failed for {}", name);
            return false;
        }
        REX::INFO("InstallGFXHooks_Internal: {} hook installed", name);
        return true;
    };

    auto* contextVTable = *reinterpret_cast<void***>(g_rendererData->context);
    if (!installVTableHook(contextVTable, 12, reinterpret_cast<void*>(MyDrawIndexed), D3D11Hooks::OriginalDrawIndexed, "DrawIndexed") ||
        !installVTableHook(contextVTable, 13, reinterpret_cast<void*>(MyDraw), D3D11Hooks::OriginalDraw, "Draw") ||
        !installVTableHook(contextVTable, 14, reinterpret_cast<void*>(MyMap), D3D11Hooks::OriginalMap, "Map") ||
        !installVTableHook(contextVTable, 15, reinterpret_cast<void*>(MyUnmap), D3D11Hooks::OriginalUnmap, "Unmap") ||
        !installVTableHook(contextVTable, 16, reinterpret_cast<void*>(MyPSSetConstantBuffers), D3D11Hooks::OriginalPSSetConstantBuffers, "PSSetConstantBuffers") ||
        !installVTableHook(contextVTable, 17, reinterpret_cast<void*>(MyIASetInputLayout), D3D11Hooks::OriginalIASetInputLayout, "IASetInputLayout") ||
        !installVTableHook(contextVTable, 18, reinterpret_cast<void*>(MyIASetVertexBuffers), D3D11Hooks::OriginalIASetVertexBuffers, "IASetVertexBuffers") ||
        !installVTableHook(contextVTable, 19, reinterpret_cast<void*>(MyIASetIndexBuffer), D3D11Hooks::OriginalIASetIndexBuffer, "IASetIndexBuffer") ||
        !installVTableHook(contextVTable, 20, reinterpret_cast<void*>(MyDrawIndexedInstanced), D3D11Hooks::OriginalDrawIndexedInstanced, "DrawIndexedInstanced") ||
        !installVTableHook(contextVTable, 21, reinterpret_cast<void*>(MyDrawInstanced), D3D11Hooks::OriginalDrawInstanced, "DrawInstanced") ||
        !installVTableHook(contextVTable, 7, reinterpret_cast<void*>(MyVSSetConstantBuffers), D3D11Hooks::OriginalVSSetConstantBuffers, "VSSetConstantBuffers") ||
        !installVTableHook(contextVTable, 8, reinterpret_cast<void*>(MyPSSetShaderResources), D3D11Hooks::OriginalPSSetShaderResources, "PSSetShaderResources") ||
        !installVTableHook(contextVTable, 10, reinterpret_cast<void*>(MyPSSetSamplers), D3D11Hooks::OriginalPSSetSamplers, "PSSetSamplers") ||
        !installVTableHook(contextVTable, 33, reinterpret_cast<void*>(MyOMSetRenderTargets), D3D11Hooks::OriginalOMSetRenderTargets, "OMSetRenderTargets") ||
        !installVTableHook(contextVTable, 35, reinterpret_cast<void*>(MyOMSetBlendState), D3D11Hooks::OriginalOMSetBlendState, "OMSetBlendState") ||
        !installVTableHook(contextVTable, 36, reinterpret_cast<void*>(MyOMSetDepthStencilState), D3D11Hooks::OriginalOMSetDepthStencilState, "OMSetDepthStencilState") ||
        !installVTableHook(contextVTable, 43, reinterpret_cast<void*>(MyRSSetState), D3D11Hooks::OriginalRSSetState, "RSSetState") ||
        !installVTableHook(contextVTable, 53, reinterpret_cast<void*>(MyClearDepthStencilView), D3D11Hooks::OriginalClearDepthStencilView, "ClearDepthStencilView") ||
        !installVTableHook(contextVTable, 9, reinterpret_cast<void*>(MyPSSetShader), D3D11Hooks::OriginalPSSetShader, "PSSetShader") ||
        !installVTableHook(contextVTable, 11, reinterpret_cast<void*>(MyVSSetShader), D3D11Hooks::OriginalVSSetShader, "VSSetShader")) {
        return false;
    }

    auto* swapChain = g_rendererData->renderWindow[0].swapChain;
    auto* swapChainVTable = *reinterpret_cast<void***>(swapChain);
    if (!installVTableHook(swapChainVTable, 8, reinterpret_cast<void*>(MyPresent), D3D11Hooks::OriginalPresent, "Present")) {
        return false;
    }

#if SHADERENGINE_ENABLE_PHASE_TELEMETRY
    InstallCommandBufferResourceTelemetryHooks();
#endif

    REX::INFO("InstallGFXHooks_Internal: All Hooks installed successfully");
    if (!DEVGUI_ON && !SHADERSETTINGS_ON) {
        REX::INFO("InstallGFXHooks_Internal: DEVGUI_ON and SHADERSETTINGS_ON are false, skipping ImGui initialization");
        return true;
    }

    HWND hwnd = g_outputWindow ? g_outputWindow : FindWindowA("Fallout4", nullptr);
    return UIInitialize(hwnd);
}

bool InstallShaderCreationHooks_Internal()
{
    REX::INFO("Hooking D3D11CreateDeviceAndSwapChain");

    std::uintptr_t d3d11Addr = Hooks::Addresses::D3D11CreateDeviceAndSwapChainCall.address();
    if (REX::FModule::GetRuntimeIndex() == REX::FModule::Runtime::kOG) {
        d3d11Addr += Hooks::Offsets::D3D11CreateDeviceAndSwapChainCallOG;
    } else {
        d3d11Addr += Hooks::Offsets::D3D11CreateDeviceAndSwapChainCallAE;
    }

    REL::Relocation<std::uintptr_t> callSite{ d3d11Addr };
    D3D11Hooks::OriginalD3D11CreateDeviceAndSwapChain =
        reinterpret_cast<D3D11Hooks::D3D11CreateDeviceAndSwapChain_t>(
            callSite.write_call<5>(reinterpret_cast<std::uintptr_t>(&HookedD3D11CreateDeviceAndSwapChain)));

    D3D11Hooks::OriginalClipCursor =
        *reinterpret_cast<D3D11Hooks::ClipCursor_t*>(Hooks::Addresses::ClipCursor.address());
    Hooks::Addresses::ClipCursor.write_vfunc(0, &HookedClipCursor);

    return true;
}
