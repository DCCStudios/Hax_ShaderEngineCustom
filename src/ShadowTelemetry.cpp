#include <PCH.h>
#include "ShadowTelemetry.h"
#include "ShadowUpgrade.h"

#if SHADERENGINE_ENABLE_SHADOW_TELEMETRY || SHADERENGINE_ENABLE_SHADOW_CACHE

#include "PhaseTelemetry.h"
#include "Global.h"
#include "hooks.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <intrin.h>

namespace ShadowTelemetry {

std::atomic<Mode> g_mode{ Mode::Off };

namespace {

using RenderShadowMap_t = void (*)(void* light, void* shadowMapData);
using RenderScene_t = void (*)(void* camera, void* accumulator, bool flag);
using AccumulateFromLists_t = void (*)(void* light, void* cullingGroup);
using RendererFlush_t = void (*)(void* renderer);
using BSShaderAccumulatorCtor_t = void* (*)(void* accumulator, std::uint32_t mode);
using BSShaderAccumulatorClearActivePasses_t = void (*)(void* accumulator, bool clearActiveLists);
using BSShaderAccumulatorClearRenderPasses_t = void (*)(void* accumulator);
using BSShaderAccumulatorSetRenderMode_t = void (*)(void* accumulator, std::uint32_t mode);
using BSShaderAccumulatorSetShadowSceneNode_t = void (*)(void* accumulator, void* sceneNode);
using BSShaderAccumulatorSetDepthPassIndex_t = void (*)(void* accumulator, std::uint32_t index);
using BSShaderAccumulatorSetShadowLight_t = void (*)(void* accumulator, void* light);
using RenderTargetManagerDestroyRenderTargets_t = void (*)(void* renderTargetManager);

RenderShadowMap_t s_origRenderShadowMap = nullptr;
RenderScene_t     s_origRenderScene = nullptr;
AccumulateFromLists_t s_origAccumulateFromLists = nullptr;
RendererFlush_t s_origRenderShadowMapFlush = nullptr;
RenderTargetManagerDestroyRenderTargets_t s_origDestroyRenderTargets = nullptr;
bool              s_installed = false;

REL::Relocation<std::uintptr_t> ptr_ShadowDirectionalRender{ REL::ID{ 871921, 2319335 } };
REL::Relocation<std::uintptr_t> ptr_RenderShadowMap{ REL::ID{ 1144068, 2319309 } };
REL::Relocation<std::uintptr_t> ptr_AccumulateFromLists{ REL::ID{ 1390075, 0 } };
REL::Relocation<BSShaderAccumulatorCtor_t> ptr_BSShaderAccumulatorCtor{ REL::ID{ 690952, 2317851 } };
REL::Relocation<BSShaderAccumulatorClearActivePasses_t> ptr_ClearActivePasses{ REL::ID{ 596187, 0 } };
REL::Relocation<BSShaderAccumulatorClearRenderPasses_t> ptr_ClearRenderPasses{ REL::ID{ 659, 0 } };
REL::Relocation<BSShaderAccumulatorSetRenderMode_t> ptr_SetRenderMode{ REL::ID{ 320514, 0 } };
REL::Relocation<BSShaderAccumulatorSetShadowSceneNode_t> ptr_SetShadowSceneNode{ REL::ID{ 1198275, 0 } };
REL::Relocation<BSShaderAccumulatorSetDepthPassIndex_t> ptr_SetDepthPassIndex{ REL::ID{ 695248, 0 } };
REL::Relocation<BSShaderAccumulatorSetShadowLight_t> ptr_SetShadowLight{ REL::ID{ 50100, 2317901 } };
REL::Relocation<std::uintptr_t> ptr_DestroyRenderTargets{ REL::ID{ 456166, 0 } };
REL::Relocation<std::uint32_t*> ptr_BSGraphicsTLSIndex{ REL::ID{ 54676, 0 }, 0x74 };
REL::Relocation<void**> ptr_BSGraphicsDefaultContext{ REL::ID{ 33539, 0 } };

constexpr std::uintptr_t kDirectionalRenderShadowMapCallOffsetOG = 0x48;  // 0x1428CA758 - 0x1428CA710
constexpr std::uintptr_t kRenderShadowMapFlushCallOffsetOG = 0xDE;        // 0x1428C98DE - 0x1428C9800
constexpr std::uintptr_t kRenderSceneCallOffsetOG = 0xFE;                 // 0x1428C98FE - 0x1428C9800

constexpr std::uintptr_t kDirectionalReturnOffsetOG = 0x4D;               // 0x1428CA75D - 0x1428CA710
constexpr std::uint32_t kMaxDirectionalCacheSplits = 4;
constexpr std::uint32_t kInvalidSplitIndex = (std::numeric_limits<std::uint32_t>::max)();
constexpr std::size_t kShadowMapDataArrayOffset = 0x198;
constexpr std::size_t kShadowMapDataCountOffset = 0x190;
constexpr std::size_t kShadowMapDataStride = 0xF0;
constexpr std::size_t kShadowMapAccumulatorOffset = 0x48;
constexpr std::size_t kShadowMapDepthTargetOffset = 0x50;
constexpr std::size_t kShadowMapMapSlotOffset = 0x54;
constexpr std::size_t kAccumulatorBatchRendererOffset = 0xC8;
constexpr std::size_t kAccumulatorSize = 0x590;
constexpr std::size_t kAccumulatorRefCountOffset = 0x08;
constexpr std::size_t kAccumulatorActiveShadowSceneNodeOffset = 0x558;
constexpr std::size_t kAccumulatorRenderModeOffset = 0x560;
constexpr std::size_t kAccumulatorDepthPassIndexOffset = 0x580;
constexpr std::uint32_t kShadowAccumulatorCtorMode = 0x63;
constexpr std::uint32_t kShadowRenderMode = 0x10;
constexpr std::size_t kAccumulateFromListsPrologueSizeOG = 15;
constexpr std::size_t kDestroyRenderTargetsPrologueSizeOG = 5;
constexpr std::size_t kBSGraphicsTLSD3DContextOffset = 0xB18;
constexpr std::size_t kBSGraphicsTLSContextOffset = 0xB20;
constexpr std::size_t kBSGraphicsContextShadowStateOffset = 0x1B70;
constexpr std::size_t kRendererStateDepthPlatformTargetOffset = 0x4C;
constexpr std::size_t kRendererStateDepthMapSlotOffset = 0x50;
constexpr std::size_t kRendererStateDepthLogicalTargetOffset = 0x88;
constexpr std::uint32_t kFirstCacheableDirectionalMapSlot = 0;
constexpr bool kShadowCacheLogEnabled = kDetailedShadowCacheLogging;

constexpr double kLogIntervalSecs = 2.0;
constexpr std::size_t kMaxLoggedKeys = 8;
constexpr std::size_t kMaxLoggedWorkKeys = 6;
constexpr std::size_t kWorkKindCount = 3;

enum class LightKind : std::uint8_t {
    Unknown,
    Directional,
    Parabolic,
    Frustum,
};

const char* LightKindName(LightKind kind) noexcept
{
    switch (kind) {
    case LightKind::Directional: return "directional";
    case LightKind::Parabolic:   return "parabolic";
    case LightKind::Frustum:     return "frustum";
    default:                     return "unknown";
    }
}

const char* WorkKindName(WorkKind kind) noexcept
{
    switch (kind) {
    case WorkKind::CommandBufferReplay: return "cmdReplay";
    case WorkKind::ImmediatePass:       return "immediate";
    case WorkKind::BuildCommandBuffer:  return "buildCB";
    default:                            return "unknown";
    }
}

LightKind ClassifyCaller(void* returnAddress) noexcept
{
    const auto ra = reinterpret_cast<std::uintptr_t>(returnAddress);
    if (ra == ptr_ShadowDirectionalRender.address() + kDirectionalReturnOffsetOG) return LightKind::Directional;
    return LightKind::Unknown;
}

template <class T>
T ReadField(void* base, std::size_t offset, T fallback = {}) noexcept
{
    if (!base) return fallback;
    T value{};
    std::memcpy(&value, static_cast<std::byte*>(base) + offset, sizeof(T));
    return value;
}

template <class T>
void WriteField(void* base, std::size_t offset, const T& value) noexcept
{
    if (!base) {
        return;
    }
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof(T));
}

std::uint64_t HashBytes(const void* data, std::size_t size) noexcept
{
    if (!data || size == 0) {
        return 0;
    }

    constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    std::uint64_t hash = kFnvOffset;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

std::int32_t QuantizeFloat(float value, float step) noexcept
{
    if (!std::isfinite(value) || step <= 0.0f) {
        return (std::numeric_limits<std::int32_t>::min)();
    }

    const float q = std::round(value / step);
    if (q < static_cast<float>((std::numeric_limits<std::int32_t>::min)() + 1)) {
        return (std::numeric_limits<std::int32_t>::min)() + 1;
    }
    if (q > static_cast<float>((std::numeric_limits<std::int32_t>::max)())) {
        return (std::numeric_limits<std::int32_t>::max)();
    }
    return static_cast<std::int32_t>(q);
}

void* ActiveRendererShadowState() noexcept;

std::uint64_t ShadowCameraSignature(void* camera, std::uint32_t mapSlot) noexcept
{
    if (!camera) {
        return 0;
    }

    // RE::NiCamera: worldToCam @ 0x120, viewFrustum/min/far @ 0x160..0x183,
    // port @ 0x184..0x193. IDA shows directional UpdateCamerasI rewrites both
    // the per-split frustum and port before culling/rendering.
    // Keep this conservative: if the directional shadow camera moves or rotates
    // enough for the static depth projection to visibly shift, rebuild instead
    // of reusing stale depth. Tiny quantization absorbs float noise only.
    constexpr std::size_t kWorldToCamOffset = 0x120;
    constexpr std::size_t kFrustumOffset = 0x160;
    constexpr std::size_t kFrustumFloatCount = 9;  // NiFrustum + minNear + maxFarNearRatio
    constexpr std::size_t kPortOffset = 0x184;
    constexpr std::size_t kPortFloatCount = 4;
    constexpr float kNearRotationStep = 0.0001f;
    constexpr float kFarRotationStep = 0.1000f;
    constexpr float kNearTranslationStep = 0.005f;
    constexpr float kFarTranslationStep = 12.0f;
    constexpr float kNearFrustumStep = 0.0001f;
    constexpr float kFarFrustumStep = 4.0f;
    constexpr float kNearPortStep = 0.0001f;
    constexpr float kFarPortStep = 0.005f;
    const float rotationStep = mapSlot == 0 ? kNearRotationStep : kFarRotationStep;
    const float translationStep = mapSlot == 0 ? kNearTranslationStep : kFarTranslationStep;
    const float frustumStep = mapSlot == 0 ? kNearFrustumStep : kFarFrustumStep;
    const float portStep = mapSlot == 0 ? kNearPortStep : kFarPortStep;

    std::array<std::int32_t, 16 + kFrustumFloatCount + kPortFloatCount> quantized{};
    const auto* matrix = reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + kWorldToCamOffset);
    for (std::size_t i = 0; i < 16; ++i) {
        const float v = matrix[i];
        const float step = std::abs(v) <= 4.0f ? rotationStep : translationStep;
        quantized[i] = QuantizeFloat(v, step);
    }

    const auto* frustum = reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + kFrustumOffset);
    for (std::size_t i = 0; i < kFrustumFloatCount; ++i) {
        quantized[16 + i] = QuantizeFloat(frustum[i], frustumStep);
    }

    const auto* port = reinterpret_cast<const float*>(static_cast<const std::byte*>(camera) + kPortOffset);
    for (std::size_t i = 0; i < kPortFloatCount; ++i) {
        quantized[16 + kFrustumFloatCount + i] = QuantizeFloat(port[i], portStep);
    }

    return HashBytes(quantized.data(), quantized.size() * sizeof(quantized[0]));
}

std::uint64_t GameplayCameraTranslationSignature() noexcept
{
    constexpr float kGameplayCameraTranslationStep = 0.01f;
    const std::array<std::int32_t, 3> quantized{
        QuantizeFloat(g_customBufferData.cameraWorldRow3.x, kGameplayCameraTranslationStep),
        QuantizeFloat(g_customBufferData.cameraWorldRow3.y, kGameplayCameraTranslationStep),
        QuantizeFloat(g_customBufferData.cameraWorldRow3.z, kGameplayCameraTranslationStep),
    };
    return HashBytes(quantized.data(), quantized.size() * sizeof(quantized[0]));
}

std::uint64_t ShadowLightDirectionSignature(void* light, std::uint32_t mapSlot) noexcept
{
    if (!light) {
        return 0;
    }

    const float x = ReadField<float>(light, 0x200);
    const float y = ReadField<float>(light, 0x204);
    const float z = ReadField<float>(light, 0x208);
    const float lenSq = x * x + y * y + z * z;
    if (!std::isfinite(lenSq) || lenSq <= 1.0e-6f) {
        return 0;
    }

    constexpr float kNearSunDirectionStep = 0.00001f;
    constexpr float kFarSunDirectionStep = 0.001f;
    constexpr float kNearSunBlendStep = 0.00001f;
    constexpr float kFarSunBlendStep = 0.001f;
    const float sunDirectionStep = mapSlot == 0 ? kNearSunDirectionStep : kFarSunDirectionStep;
    const float sunBlendStep = mapSlot == 0 ? kNearSunBlendStep : kFarSunBlendStep;
    const float invLen = 1.0f / std::sqrt(lenSq);
    const std::array<std::int32_t, 4> quantized{
        QuantizeFloat(x * invLen, sunDirectionStep),
        QuantizeFloat(y * invLen, sunDirectionStep),
        QuantizeFloat(z * invLen, sunDirectionStep),
        QuantizeFloat(ReadField<float>(light, 0x220), sunBlendStep),
    };
    return HashBytes(quantized.data(), quantized.size() * sizeof(quantized[0]));
}

std::uint32_t ShadowMapDataIndex(void* light, void* shadowMapData) noexcept
{
    if (!light || !shadowMapData) {
        return (std::numeric_limits<std::uint32_t>::max)();
    }

    auto* base = ReadField<std::byte*>(light, kShadowMapDataArrayOffset);
    auto* cur = static_cast<std::byte*>(shadowMapData);
    if (!base || cur < base) {
        return (std::numeric_limits<std::uint32_t>::max)();
    }

    const auto delta = static_cast<std::uintptr_t>(cur - base);
    if ((delta % kShadowMapDataStride) != 0) {
        return (std::numeric_limits<std::uint32_t>::max)();
    }
    return static_cast<std::uint32_t>(delta / kShadowMapDataStride);
}

struct ViewportSig {
    std::uint32_t d0 = 0;
    std::uint32_t d4 = 0;
    std::uint32_t d8 = 0;
    std::uint32_t dc = 0;

    bool operator==(const ViewportSig& o) const noexcept
    {
        return d0 == o.d0 && d4 == o.d4 && d8 == o.d8 && dc == o.dc;
    }
};

struct Key {
    void* light = nullptr;
    void* accumulator = nullptr;
    void* activeDepthStencilView = nullptr;
    void* activeDepthTexture = nullptr;
    std::uint64_t cameraSig = 0;
    std::uint64_t dominantLightSig = 0;
    std::uint32_t depthTarget = 0;
    std::uint32_t mapSlot = 0;
    ViewportSig viewport{};
    LightKind kind = LightKind::Unknown;

    bool operator==(const Key& o) const noexcept
    {
        return light == o.light &&
               accumulator == o.accumulator &&
               cameraSig == o.cameraSig &&
               dominantLightSig == o.dominantLightSig &&
               depthTarget == o.depthTarget &&
               mapSlot == o.mapSlot &&
               viewport == o.viewport &&
               kind == o.kind;
    }
};

struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept
    {
        std::size_t h = std::hash<void*>{}(k.light);
        auto mix = [&h](std::size_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(std::hash<void*>{}(k.accumulator));
        mix(std::hash<std::uint64_t>{}(k.cameraSig));
        mix(std::hash<std::uint64_t>{}(k.dominantLightSig));
        mix(std::hash<std::uint32_t>{}(k.depthTarget));
        mix(std::hash<std::uint32_t>{}(k.mapSlot));
        mix(std::hash<std::uint32_t>{}(k.viewport.d0));
        mix(std::hash<std::uint32_t>{}(k.viewport.d4));
        mix(std::hash<std::uint32_t>{}(k.viewport.d8));
        mix(std::hash<std::uint32_t>{}(k.viewport.dc));
        mix(std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(k.kind)));
        return h;
    }
};

REX::W32::ID3D11DepthStencilView* ActiveShadowDSV(const Key& key) noexcept
{
    if (!g_rendererData ||
        key.mapSlot >= 4) {
        return nullptr;
    }

    const auto renderTargetManager = RE::BSGraphics::RenderTargetManager::GetSingleton();
    if (key.depthTarget >= static_cast<std::uint32_t>(std::size(renderTargetManager.depthStencilTargetID))) {
        return nullptr;
    }

    const std::uint32_t platformDepthTarget = renderTargetManager.depthStencilTargetID[key.depthTarget];
    if (platformDepthTarget >= static_cast<std::uint32_t>(std::size(g_rendererData->depthStencilTargets))) {
        return nullptr;
    }

    return g_rendererData->depthStencilTargets[platformDepthTarget].dsView[key.mapSlot];
}

REX::W32::ID3D11DeviceContext* ActiveD3DContext() noexcept
{
    void* context = nullptr;
    const auto tlsIndex = *ptr_BSGraphicsTLSIndex;
        auto** tlsSlots = reinterpret_cast<void**>(__readgsqword(0x58));
        auto* tlsBase = tlsSlots ? static_cast<std::byte*>(tlsSlots[tlsIndex]) : nullptr;
        context = ReadField<void*>(tlsBase, kBSGraphicsTLSD3DContextOffset);


    if (context) {
        return static_cast<REX::W32::ID3D11DeviceContext*>(context);
    }

    return g_rendererData ? g_rendererData->context : nullptr;
}

REX::W32::ID3D11DepthStencilView* RendererStateDepthStencilView(
    std::uint32_t platformDepthTarget,
    std::uint32_t mapSlot) noexcept
{
    if (!g_rendererData ||
        platformDepthTarget >= static_cast<std::uint32_t>(std::size(g_rendererData->depthStencilTargets)) ||
        mapSlot >= 4) {
        return nullptr;
    }

    return g_rendererData->depthStencilTargets[platformDepthTarget].dsView[mapSlot];
}

REX::W32::ID3D11DepthStencilView* ActiveShadowDSVFromRendererState(const Key& key) noexcept
{
    if (auto* dsv = ActiveShadowDSV(key)) {
        return dsv;
    }

    auto* state = ActiveRendererShadowState();
    if (!state || key.mapSlot >= 4) {
        return nullptr;
    }

    const std::uint32_t logicalDepthTarget =
        ReadField<std::uint32_t>(state, kRendererStateDepthLogicalTargetOffset, kInvalidSplitIndex);
    const std::uint32_t platformDepthTarget =
        ReadField<std::uint32_t>(state, kRendererStateDepthPlatformTargetOffset, kInvalidSplitIndex);
    const std::uint32_t mapSlot =
        ReadField<std::uint32_t>(state, kRendererStateDepthMapSlotOffset, kInvalidSplitIndex);

    if (mapSlot != key.mapSlot) {
        return nullptr;
    }

    const auto renderTargetManager = RE::BSGraphics::RenderTargetManager::GetSingleton();
    const bool logicalMatches = logicalDepthTarget == key.depthTarget;
    const bool platformMatches =
        key.depthTarget < static_cast<std::uint32_t>(std::size(renderTargetManager.depthStencilTargetID)) &&
        renderTargetManager.depthStencilTargetID[key.depthTarget] == platformDepthTarget;
    if (!logicalMatches && !platformMatches) {
        return nullptr;
    }

    return RendererStateDepthStencilView(platformDepthTarget, mapSlot);
}

void* ActiveRendererShadowState() noexcept
{
    void* context = nullptr;
    const auto tlsIndex = *ptr_BSGraphicsTLSIndex;
        auto** tlsSlots = reinterpret_cast<void**>(__readgsqword(0x58));
        auto* tlsBase = tlsSlots ? static_cast<std::byte*>(tlsSlots[tlsIndex]) : nullptr;
        context = ReadField<void*>(tlsBase, kBSGraphicsTLSContextOffset);


    if (!context) {
        context = *ptr_BSGraphicsDefaultContext;
    }

    if (context) {
        return static_cast<std::byte*>(context) + kBSGraphicsContextShadowStateOffset;
    }

    return g_rendererData ? g_rendererData->shadowState : nullptr;
}

struct Aggregate {
    std::uint64_t calls = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxNs = 0;
    std::uint64_t bsDraws = 0;
    std::uint64_t d3dDraws = 0;
    std::uint64_t cmdBufDraws = 0;
    std::uint64_t renderSceneCalls = 0;
    std::uint64_t renderSceneNs = 0;
    std::uint64_t deferredLightsCalls = 0;
};

struct ShadowCacheState {
    Key key{};
    Key pendingBuildKey{};
    void* cullingProcess = nullptr;
    std::uint32_t skipsSinceUpdate = 0;
    std::uint32_t stableKeyHits = 0;
    std::uint32_t pendingBuildKeyHits = 0;
    std::uint64_t eligible = 0;
    std::uint64_t updates = 0;
    std::uint64_t skips = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t staticBuilds = 0;
    std::uint64_t restoreFailures = 0;
    std::uint64_t captured = 0;
    std::uint64_t emptyStaticBuilds = 0;
    std::uint64_t staticKept = 0;
    std::uint64_t staticSkipped = 0;
    std::uint64_t overlayKept = 0;
    std::uint64_t overlaySkipped = 0;
    std::uint64_t shadowMapOrMaskHookCalls = 0;
    std::uint64_t shadowMapOrMaskHookActiveCalls = 0;
    std::array<std::uint64_t, 6> shadowMapOrMaskHookCallsByPass{};
    std::array<std::uint64_t, 6> shadowMapOrMaskHookActiveCallsByPass{};
    bool valid = false;
};

enum class ShadowCachePass : std::uint8_t {
    None,
    Passthrough,
    BuildBoth,
    DynamicRegister,
    StaticRender,
    DynamicRender,
};

struct StaticDepthCache {
    REX::W32::ID3D11Texture2D* texture = nullptr;
    REX::W32::D3D11_TEXTURE2D_DESC desc{};
    std::uint64_t validSubresources = 0;
    bool valid = false;
};

struct DirectionalSplitContext {
    void* sunLight = nullptr;
    void* shadowMapData = nullptr;
    void* vanillaAccumulator = nullptr;
    void* staticAccumulator = nullptr;
    void* dynamicAccumulator = nullptr;
    Key key{};
    std::uint32_t slotIndex = kInvalidSplitIndex;
    ShadowCachePass phase = ShadowCachePass::None;
    bool eligible = false;
    bool cacheHit = false;
    bool buildBoth = false;
    bool hitRouted = false;
    bool staticRenderedThisFrame = false;
    bool staticCaptureSucceeded = false;
    bool staticCaptureAttempted = false;
    bool restoreFailedThisFrame = false;
    bool fullFallbackThisFrame = false;
    bool dynamicRestoreAttempted = false;
    bool dynamicRestoreSucceeded = false;
    bool splitFailed = false;
    bool splitEmpty = false;
    std::uint32_t passthroughCooldown = 0;
    std::uint32_t buildStaticRouted = 0;
    REX::W32::ID3D11DepthStencilView* activeDepthStencilView = nullptr;
};

struct WorkAggregate {
    std::uint64_t calls = 0;
    std::uint64_t totalNs = 0;
    std::uint64_t maxNs = 0;
    std::uint64_t bsDraws = 0;
    std::uint64_t d3dDraws = 0;
    std::uint64_t cmdBufDraws = 0;
};

struct WorkKey {
    Key shadow{};
    WorkKind kind = WorkKind::CommandBufferReplay;
    void* owner = nullptr;
    void* head = nullptr;
    void* cbData = nullptr;
    void* commandBuffer = nullptr;
    void* geometry = nullptr;
    void* shader = nullptr;
    void* shaderProperty = nullptr;
    std::uint32_t techniqueID = 0;
    std::int32_t passGroupIdx = -1;
    std::uint32_t subIdx = 0;
    std::uint32_t chainLen = 0;
    std::uint32_t srvCount = 0;
    std::uint32_t cbRecordCount = 0;
    std::uint32_t cbDrawCount = 0;
    std::uint32_t cbNonZeroDrawCount = 0;
    std::uint32_t cbMaxDrawCount = 0;
    std::uint32_t cbSrvRecordCount = 0;
    std::uint32_t cbMaxSrvRecordCount = 0;
    bool allowAlpha = false;

    bool operator==(const WorkKey& o) const noexcept
    {
        return shadow == o.shadow &&
               kind == o.kind &&
               owner == o.owner &&
               head == o.head &&
               cbData == o.cbData &&
               commandBuffer == o.commandBuffer &&
               geometry == o.geometry &&
               shader == o.shader &&
               shaderProperty == o.shaderProperty &&
               techniqueID == o.techniqueID &&
               passGroupIdx == o.passGroupIdx &&
               subIdx == o.subIdx &&
               chainLen == o.chainLen &&
               srvCount == o.srvCount &&
               cbRecordCount == o.cbRecordCount &&
               cbDrawCount == o.cbDrawCount &&
               cbNonZeroDrawCount == o.cbNonZeroDrawCount &&
               cbMaxDrawCount == o.cbMaxDrawCount &&
               cbSrvRecordCount == o.cbSrvRecordCount &&
               cbMaxSrvRecordCount == o.cbMaxSrvRecordCount &&
               allowAlpha == o.allowAlpha;
    }
};

struct WorkKeyHash {
    std::size_t operator()(const WorkKey& k) const noexcept
    {
        std::size_t h = KeyHash{}(k.shadow);
        auto mix = [&h](std::size_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        };
        mix(std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(k.kind)));
        mix(std::hash<void*>{}(k.owner));
        mix(std::hash<void*>{}(k.head));
        mix(std::hash<void*>{}(k.cbData));
        mix(std::hash<void*>{}(k.commandBuffer));
        mix(std::hash<void*>{}(k.geometry));
        mix(std::hash<void*>{}(k.shader));
        mix(std::hash<void*>{}(k.shaderProperty));
        mix(std::hash<std::uint32_t>{}(k.techniqueID));
        mix(std::hash<std::int32_t>{}(k.passGroupIdx));
        mix(std::hash<std::uint32_t>{}(k.subIdx));
        mix(std::hash<std::uint32_t>{}(k.chainLen));
        mix(std::hash<std::uint32_t>{}(k.srvCount));
        mix(std::hash<std::uint32_t>{}(k.cbRecordCount));
        mix(std::hash<std::uint32_t>{}(k.cbDrawCount));
        mix(std::hash<std::uint32_t>{}(k.cbNonZeroDrawCount));
        mix(std::hash<std::uint32_t>{}(k.cbMaxDrawCount));
        mix(std::hash<std::uint32_t>{}(k.cbSrvRecordCount));
        mix(std::hash<std::uint32_t>{}(k.cbMaxSrvRecordCount));
        mix(std::hash<bool>{}(k.allowAlpha));
        return h;
    }
};

struct Scope {
    Key key{};
    void* shadowMapData = nullptr;
    void* camera = nullptr;
    void* cullingProcess = nullptr;
    std::uint32_t shadowMapDataIndex = (std::numeric_limits<std::uint32_t>::max)();
    std::chrono::steady_clock::time_point start{};
    std::uint64_t bsDraws = 0;
    std::uint64_t d3dDraws = 0;
    std::uint64_t cmdBufDraws = 0;
    std::uint64_t renderSceneCalls = 0;
    std::uint64_t renderSceneNs = 0;
    std::array<WorkAggregate, kWorkKindCount> work{};
    bool inDeferredLights = false;
};

thread_local std::vector<Scope> tl_scopes;

struct WorkScope {
    WorkKey key{};
    std::chrono::steady_clock::time_point start{};
    std::uint64_t bsDraws = 0;
    std::uint64_t d3dDraws = 0;
    std::uint64_t cmdBufDraws = 0;
};

thread_local std::vector<WorkScope> tl_workScopes;
Aggregate s_total;
std::unordered_map<Key, Aggregate, KeyHash> s_byKey;
std::array<WorkAggregate, kWorkKindCount> s_workTotals{};
std::unordered_map<WorkKey, WorkAggregate, WorkKeyHash> s_workByKey;
std::chrono::steady_clock::time_point s_lastLogTime;
std::chrono::steady_clock::time_point s_lastCacheLogTime;
std::array<ShadowCacheState, kMaxDirectionalCacheSplits> s_directionalSplitCaches{};
std::array<StaticDepthCache, kMaxDirectionalCacheSplits> s_staticDepthCaches{};
std::array<DirectionalSplitContext, kMaxDirectionalCacheSplits> s_directionalSplitContexts{};
struct alignas(16) AccumulatorStorage {
    std::byte bytes[kAccumulatorSize]{};
};
std::array<AccumulatorStorage, kMaxDirectionalCacheSplits> s_staticAccumulatorStorage{};
std::array<AccumulatorStorage, kMaxDirectionalCacheSplits> s_dynamicAccumulatorStorage{};
std::array<bool, kMaxDirectionalCacheSplits> s_staticAccumulatorConstructed{};
std::array<bool, kMaxDirectionalCacheSplits> s_dynamicAccumulatorConstructed{};
thread_local ShadowCachePass tl_shadowCachePass = ShadowCachePass::None;
thread_local std::uint32_t tl_shadowCacheRenderSplit = kInvalidSplitIndex;
thread_local std::uint32_t tl_lastRegistrationSplit = kInvalidSplitIndex;
std::atomic<ShadowCachePass> g_registrationShadowCachePass{ ShadowCachePass::None };
std::atomic_bool g_registrationShadowCacheActive{ false };
std::array<std::atomic_uint32_t, kMaxDirectionalCacheSplits> g_registrationStaticKeepCounts{};
std::array<std::atomic_uint32_t, kMaxDirectionalCacheSplits> g_registrationDynamicKeepCounts{};
std::array<std::atomic<void*>, kMaxDirectionalCacheSplits> g_registrationShadowCacheAccumulators{};
std::array<std::atomic<void*>, kMaxDirectionalCacheSplits> g_registrationStaticAccumulators{};
std::array<std::atomic<void*>, kMaxDirectionalCacheSplits> g_registrationDynamicAccumulators{};
std::array<std::atomic_bool, kMaxDirectionalCacheSplits> g_registrationBuildBothBySplit{};
std::array<std::atomic_bool, kMaxDirectionalCacheSplits> g_registrationDynamicOnlyBySplit{};
std::atomic_bool g_shadowCacheTargetActive{ false };
std::atomic<std::uint32_t> g_shadowCacheTargetMapSlot{ (std::numeric_limits<std::uint32_t>::max)() };
std::atomic<std::uint32_t> g_shadowCacheTargetDepthTarget{ (std::numeric_limits<std::uint32_t>::max)() };
std::atomic<std::uint32_t> g_shadowCacheTargetSplit{ kInvalidSplitIndex };
std::atomic_uint64_t g_shadowCacheClearRejectTargetInactive{ 0 };
std::atomic_uint64_t g_shadowCacheClearRejectSplitInvalid{ 0 };
std::atomic_uint64_t g_shadowCacheClearRejectDSVMismatch{ 0 };

class ScopedOMUnbind
{
public:
    explicit ScopedOMUnbind(REX::W32::ID3D11DeviceContext* context) :
        context_(context)
    {
        if (!context_) {
            return;
        }

        context_->OMGetRenderTargets(kMaxRTVs, rtvs_.data(), &dsv_);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }

    ~ScopedOMUnbind()
    {
        if (!context_) {
            return;
        }

        context_->OMSetRenderTargets(kMaxRTVs, rtvs_.data(), dsv_);
        for (auto*& rtv : rtvs_) {
            if (rtv) {
                rtv->Release();
                rtv = nullptr;
            }
        }
        if (dsv_) {
            dsv_->Release();
            dsv_ = nullptr;
        }
    }

    ScopedOMUnbind(const ScopedOMUnbind&) = delete;
    ScopedOMUnbind& operator=(const ScopedOMUnbind&) = delete;

private:
    static constexpr std::uint32_t kMaxRTVs = 8;
    REX::W32::ID3D11DeviceContext* context_ = nullptr;
    std::array<REX::W32::ID3D11RenderTargetView*, kMaxRTVs> rtvs_{};
    REX::W32::ID3D11DepthStencilView* dsv_ = nullptr;
};

std::size_t WorkIndex(WorkKind kind) noexcept
{
    return static_cast<std::size_t>(kind) < kWorkKindCount ? static_cast<std::size_t>(kind) : 0;
}

void AddToWorkAggregate(WorkAggregate& dst, std::uint64_t ns, std::uint64_t bsDraws, std::uint64_t d3dDraws, std::uint64_t cmdBufDraws)
{
    ++dst.calls;
    dst.totalNs += ns;
    if (ns > dst.maxNs) {
        dst.maxNs = ns;
    }
    dst.bsDraws += bsDraws;
    dst.d3dDraws += d3dDraws;
    dst.cmdBufDraws += cmdBufDraws;
}

void AddToAggregate(Aggregate& dst, const Scope& scope, std::uint64_t ns)
{
    ++dst.calls;
    dst.totalNs += ns;
    if (ns > dst.maxNs) {
        dst.maxNs = ns;
    }
    dst.bsDraws += scope.bsDraws;
    dst.d3dDraws += scope.d3dDraws;
    dst.cmdBufDraws += scope.cmdBufDraws;
    dst.renderSceneCalls += scope.renderSceneCalls;
    dst.renderSceneNs += scope.renderSceneNs;
    if (scope.inDeferredLights) {
        ++dst.deferredLightsCalls;
    }
}

void ResetWindow()
{
    s_total = {};
    s_byKey.clear();
    s_workTotals = {};
    s_workByKey.clear();
    for (auto& cache : s_directionalSplitCaches) {
        cache.updates = 0;
        cache.skips = 0;
        cache.invalidations = 0;
        cache.hits = 0;
        cache.misses = 0;
        cache.staticBuilds = 0;
        cache.restoreFailures = 0;
        cache.captured = 0;
        cache.emptyStaticBuilds = 0;
        cache.staticKept = 0;
        cache.staticSkipped = 0;
        cache.overlayKept = 0;
        cache.overlaySkipped = 0;
        cache.shadowMapOrMaskHookCalls = 0;
        cache.shadowMapOrMaskHookActiveCalls = 0;
        cache.shadowMapOrMaskHookCallsByPass = {};
        cache.shadowMapOrMaskHookActiveCallsByPass = {};
    }
}

void SetRegistrationShadowCachePass(ShadowCachePass pass) noexcept;
void IncrementRegistrationCounter(std::uint64_t& counter) noexcept;

std::size_t PassIndex(ShadowCachePass pass) noexcept
{
    const auto idx = static_cast<std::size_t>(pass);
    return idx < 6 ? idx : 0;
}

const char* PassShortName(ShadowCachePass pass) noexcept
{
    switch (pass) {
    case ShadowCachePass::None: return "n";
    case ShadowCachePass::Passthrough: return "p";
    case ShadowCachePass::BuildBoth: return "b";
    case ShadowCachePass::DynamicRegister: return "r";
    case ShadowCachePass::StaticRender: return "s";
    case ShadowCachePass::DynamicRender: return "d";
    default: return "?";
    }
}

bool ValidSplitIndex(std::uint32_t splitIndex) noexcept
{
    return splitIndex < kMaxDirectionalCacheSplits;
}

ShadowCacheState& CacheForSplit(std::uint32_t splitIndex) noexcept
{
    return s_directionalSplitCaches[(std::min)(splitIndex, kMaxDirectionalCacheSplits - 1)];
}

StaticDepthCache& DepthCacheForSplit(std::uint32_t splitIndex) noexcept
{
    return s_staticDepthCaches[(std::min)(splitIndex, kMaxDirectionalCacheSplits - 1)];
}

DirectionalSplitContext& ContextForSplit(std::uint32_t splitIndex) noexcept
{
    return s_directionalSplitContexts[(std::min)(splitIndex, kMaxDirectionalCacheSplits - 1)];
}

bool DSVSliceMatchesMapSlot(REX::W32::ID3D11DepthStencilView* dsv, std::uint32_t mapSlot) noexcept;

bool IsRegistrationFilteringPass(ShadowCachePass pass) noexcept
{
    return pass == ShadowCachePass::BuildBoth ||
           pass == ShadowCachePass::DynamicRegister ||
           pass == ShadowCachePass::DynamicRender;
}

std::uint32_t FindRegistrationSplitForAccumulator(void* accumulator) noexcept
{
    if (!accumulator) {
        return kInvalidSplitIndex;
    }

    for (std::uint32_t i = 0; i < kMaxDirectionalCacheSplits; ++i) {
        void* expected = g_registrationShadowCacheAccumulators[i].load(std::memory_order_acquire);
        if (expected && expected == accumulator) {
            return i;
        }
    }
    return kInvalidSplitIndex;
}

bool RegistrationAccumulatorMatches(void* accumulator) noexcept
{
    return ValidSplitIndex(FindRegistrationSplitForAccumulator(accumulator));
}

ShadowCachePass CurrentResolvedRegistrationPass() noexcept
{
    return g_registrationShadowCachePass.load(std::memory_order_acquire);
}

void ReleaseStaticDepthCache(StaticDepthCache& cache) noexcept
{
    if (cache.texture) {
        cache.texture->Release();
        cache.texture = nullptr;
    }
    cache.desc = {};
    cache.validSubresources = 0;
    cache.valid = false;
}

void ReleaseStaticDepthCaches() noexcept
{
    for (auto& cache : s_staticDepthCaches) {
        ReleaseStaticDepthCache(cache);
    }
}

bool SameTextureDesc(const REX::W32::D3D11_TEXTURE2D_DESC& a,
                     const REX::W32::D3D11_TEXTURE2D_DESC& b) noexcept
{
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool GetDSVSubresource(const REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC& dsvDesc,
                       const REX::W32::D3D11_TEXTURE2D_DESC& texDesc,
                       std::uint32_t& outSubresource) noexcept
{
    switch (dsvDesc.viewDimension) {
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2D:
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2DMS:
        outSubresource = dsvDesc.viewDimension == REX::W32::D3D11_DSV_DIMENSION_TEXTURE2D ?
            dsvDesc.texture2D.mipSlice :
            0;
        return outSubresource < texDesc.mipLevels * texDesc.arraySize;
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        if (dsvDesc.texture2DArray.arraySize != 1) {
            return false;
        }
        outSubresource = dsvDesc.texture2DArray.mipSlice +
                         dsvDesc.texture2DArray.firstArraySlice * texDesc.mipLevels;
        return outSubresource < texDesc.mipLevels * texDesc.arraySize;
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        if (dsvDesc.texture2DMSArray.arraySize != 1) {
            return false;
        }
        outSubresource = dsvDesc.texture2DMSArray.firstArraySlice * texDesc.mipLevels;
        return outSubresource < texDesc.mipLevels * texDesc.arraySize;
    default:
        return false;
    }
}

bool QueryTextureFromDSV(REX::W32::ID3D11DepthStencilView* dsv,
                         REX::W32::ID3D11Texture2D** outTexture,
                         REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC* outDSVDesc,
                         std::uint32_t* outSubresource)
{
    if (!dsv || !outTexture) {
        return false;
    }

    REX::W32::ID3D11Resource* resource = nullptr;
    dsv->GetResource(&resource);
    if (!resource) {
        return false;
    }

    REX::W32::ID3D11Texture2D* texture = nullptr;
    const HRESULT qi = resource->QueryInterface(
        REX::W32::IID_ID3D11Texture2D,
        reinterpret_cast<void**>(&texture));
    resource->Release();
    if (FAILED(qi) || !texture) {
        return false;
    }

    REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsv->GetDesc(&dsvDesc);
    REX::W32::D3D11_TEXTURE2D_DESC texDesc{};
    texture->GetDesc(&texDesc);

    std::uint32_t subresource = 0;
    if (!GetDSVSubresource(dsvDesc, texDesc, subresource)) {
        texture->Release();
        return false;
    }

    if (outDSVDesc) {
        *outDSVDesc = dsvDesc;
    }
    if (outSubresource) {
        *outSubresource = subresource;
    }
    *outTexture = texture;
    return true;
}

bool DSVSliceMatchesMapSlot(REX::W32::ID3D11DepthStencilView* dsv, std::uint32_t mapSlot) noexcept
{
    REX::W32::ID3D11Texture2D* texture = nullptr;
    REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    std::uint32_t subresource = 0;
    if (!QueryTextureFromDSV(dsv, &texture, &dsvDesc, &subresource)) {
        return false;
    }

    REX::W32::D3D11_TEXTURE2D_DESC texDesc{};
    texture->GetDesc(&texDesc);
    texture->Release();

    if (texDesc.mipLevels == 0) {
        return false;
    }

    switch (dsvDesc.viewDimension) {
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2D:
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2DMS:
        return false;
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        return dsvDesc.texture2DArray.arraySize == 1 &&
               dsvDesc.texture2DArray.firstArraySlice == mapSlot &&
               subresource == dsvDesc.texture2DArray.mipSlice + mapSlot * texDesc.mipLevels;
    case REX::W32::D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        return dsvDesc.texture2DMSArray.arraySize == 1 &&
               dsvDesc.texture2DMSArray.firstArraySlice == mapSlot &&
               subresource == mapSlot * texDesc.mipLevels;
    default:
        return false;
    }
}

void* ActiveShadowTextureIdentity(REX::W32::ID3D11DepthStencilView* dsv) noexcept
{
    REX::W32::ID3D11Texture2D* texture = nullptr;
    if (!QueryTextureFromDSV(dsv, &texture, nullptr, nullptr)) {
        return nullptr;
    }

    void* identity = texture;
    texture->Release();
    return identity;
}

bool EnsureStaticDepthCacheTexture(StaticDepthCache& cache, REX::W32::ID3D11Texture2D* activeTexture)
{
    if (!activeTexture || !g_rendererData || !g_rendererData->device) {
        return false;
    }

    REX::W32::D3D11_TEXTURE2D_DESC desc{};
    activeTexture->GetDesc(&desc);
    desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
    desc.cpuAccessFlags = 0;

    if (cache.texture && SameTextureDesc(cache.desc, desc)) {
        return true;
    }

    ReleaseStaticDepthCache(cache);
    auto* device = g_rendererData->device;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &cache.texture);
    if (FAILED(hr) || !cache.texture) {
        REX::WARN("ShadowCache: CreateTexture2D static depth cache failed 0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    cache.desc = desc;
    cache.validSubresources = 0;
    cache.valid = false;
    return true;
}

bool IsCacheableSubresource(std::uint32_t subresource) noexcept
{
    return subresource < 64;
}

std::uint64_t SubresourceMask(std::uint32_t subresource) noexcept
{
    return IsCacheableSubresource(subresource) ? (1ull << subresource) : 0ull;
}

bool CopyDSVToStaticCache(StaticDepthCache& cache,
                          REX::W32::ID3D11DeviceContext* context,
                          REX::W32::ID3D11DepthStencilView* dsv)
{
    if (!context || !dsv) {
        return false;
    }

    REX::W32::ID3D11Texture2D* activeTexture = nullptr;
    std::uint32_t subresource = 0;
    if (!QueryTextureFromDSV(dsv, &activeTexture, nullptr, &subresource)) {
        return false;
    }
    if (!IsCacheableSubresource(subresource)) {
        activeTexture->Release();
        return false;
    }

    const bool ok = EnsureStaticDepthCacheTexture(cache, activeTexture);
    if (ok) {
        ScopedOMUnbind unbind(context);
        context->CopySubresourceRegion(
            cache.texture, subresource, 0, 0, 0,
            activeTexture, subresource, nullptr);
        cache.validSubresources |= SubresourceMask(subresource);
        cache.valid = true;
    }
    activeTexture->Release();
    return ok;
}

void LogRestoreFailure(
    const char* reason,
    std::uint32_t splitIndex,
    const Key& key,
    const StaticDepthCache& cache,
    REX::W32::ID3D11DepthStencilView* dsv,
    REX::W32::ID3D11Texture2D* activeTexture,
    const REX::W32::D3D11_TEXTURE2D_DESC* activeDesc,
    std::uint32_t subresource,
    bool subresourceKnown)
{
    if constexpr (!kShadowCacheLogEnabled) {
        return;
    }

    const auto mask = subresourceKnown ? SubresourceMask(subresource) : 0ull;
    REX::W32::D3D11_TEXTURE2D_DESC cacheDesc = cache.desc;
    REX::W32::D3D11_TEXTURE2D_DESC fallbackActiveDesc{};
    if (!activeDesc && activeTexture) {
        activeTexture->GetDesc(&fallbackActiveDesc);
        activeDesc = &fallbackActiveDesc;
    }

    const auto activeWidth = activeDesc ? activeDesc->width : 0u;
    const auto activeHeight = activeDesc ? activeDesc->height : 0u;
    const auto activeMipLevels = activeDesc ? activeDesc->mipLevels : 0u;
    const auto activeArraySize = activeDesc ? activeDesc->arraySize : 0u;
    const auto activeFormat = activeDesc ? static_cast<unsigned>(activeDesc->format) : 0u;
    const auto activeBindFlags = activeDesc ? activeDesc->bindFlags : 0u;
    const auto activeMiscFlags = activeDesc ? activeDesc->miscFlags : 0u;
    const auto activeSampleCount = activeDesc ? activeDesc->sampleDesc.count : 0u;
    const auto activeSampleQuality = activeDesc ? activeDesc->sampleDesc.quality : 0u;

    REX::WARN(
        "ShadowCacheRestoreFail[split={} mapSlot={} reason={}]: dsv={} activeTex={} keyDSV={} keyTex={} cacheTex={} "
        "subresource={}/{} mask={:#x} validMask={:#x} cacheValid={} "
        "activeDesc(w/h/mips/arr/fmt/bind/misc/sample={}/{}/{}/{}/{:#x}/{:#x}/{:#x}/{}:{}) "
        "cacheDesc(w/h/mips/arr/fmt/bind/misc/sample={}/{}/{}/{}/{:#x}/{:#x}/{:#x}/{}:{})",
        splitIndex,
        key.mapSlot,
        reason ? reason : "?",
        static_cast<void*>(dsv),
        static_cast<void*>(activeTexture),
        key.activeDepthStencilView,
        key.activeDepthTexture,
        static_cast<void*>(cache.texture),
        subresourceKnown ? subresource : 0u,
        subresourceKnown,
        mask,
        cache.validSubresources,
        cache.valid,
        activeWidth,
        activeHeight,
        activeMipLevels,
        activeArraySize,
        activeFormat,
        activeBindFlags,
        activeMiscFlags,
        activeSampleCount,
        activeSampleQuality,
        cacheDesc.width,
        cacheDesc.height,
        cacheDesc.mipLevels,
        cacheDesc.arraySize,
        static_cast<unsigned>(cacheDesc.format),
        cacheDesc.bindFlags,
        cacheDesc.miscFlags,
        cacheDesc.sampleDesc.count,
        cacheDesc.sampleDesc.quality);
}

bool RestoreStaticCacheToDSV(const StaticDepthCache& cache,
                             REX::W32::ID3D11DeviceContext* context,
                             REX::W32::ID3D11DepthStencilView* dsv,
                             std::uint32_t splitIndex,
                             const Key& key)
{
    if (!context || !dsv || !cache.texture || !cache.valid) {
        LogRestoreFailure(
            !context ? "missing-context" :
            !dsv ? "missing-dsv" :
            !cache.texture ? "missing-cache-texture" :
            "cache-invalid",
            splitIndex,
            key,
            cache,
            dsv,
            nullptr,
            nullptr,
            0,
            false);
        return false;
    }

    REX::W32::ID3D11Texture2D* activeTexture = nullptr;
    std::uint32_t subresource = 0;
    if (!QueryTextureFromDSV(dsv, &activeTexture, nullptr, &subresource)) {
        LogRestoreFailure(
            "query-dsv-failed",
            splitIndex,
            key,
            cache,
            dsv,
            nullptr,
            nullptr,
            0,
            false);
        return false;
    }

    REX::W32::D3D11_TEXTURE2D_DESC activeDesc{};
    activeTexture->GetDesc(&activeDesc);
    if (!SameTextureDesc(activeDesc, cache.desc)) {
        LogRestoreFailure(
            "desc-mismatch",
            splitIndex,
            key,
            cache,
            dsv,
            activeTexture,
            &activeDesc,
            subresource,
            true);
        activeTexture->Release();
        return false;
    }
    if ((cache.validSubresources & SubresourceMask(subresource)) == 0) {
        LogRestoreFailure(
            "missing-subresource",
            splitIndex,
            key,
            cache,
            dsv,
            activeTexture,
            &activeDesc,
            subresource,
            true);
        activeTexture->Release();
        return false;
    }

    {
        ScopedOMUnbind unbind(context);
        context->CopySubresourceRegion(
            activeTexture, subresource, 0, 0, 0,
            cache.texture, subresource, nullptr);
    }
    activeTexture->Release();
    return true;
}

bool CaptureStaticCacheFromBuildDSV(std::uint32_t splitIndex, REX::W32::ID3D11DepthStencilView* dsv)
{
    auto* context = g_rendererData ? g_rendererData->context : nullptr;
    bool ok = context && dsv;
    if (ok) {
        auto& depthCache = DepthCacheForSplit(splitIndex);
        depthCache.validSubresources = 0;
        depthCache.valid = false;
        ok = CopyDSVToStaticCache(depthCache, context, dsv);
    }
    if (ok) {
        if constexpr (kShadowCacheLogEnabled) {
            ++CacheForSplit(splitIndex).captured;
        }
    }
    return ok;
}

void* ConstructScratchAccumulator(std::byte* storage, bool& constructed, std::uint32_t splitIndex)
{
    if (constructed) {
        return storage;
    }


    std::memset(storage, 0, kAccumulatorSize);
    void* accumulator = ptr_BSShaderAccumulatorCtor(storage, kShadowAccumulatorCtorMode);
    if (!accumulator) {
        return nullptr;
    }

    // The engine temporarily stores accumulators in NiPointer fields while
    // rendering. Keep one plugin-owned reference so SetAccumulator(nullptr)
    // cannot drive the count to zero and call DeleteThis on our static storage.
    WriteField<std::uint32_t>(accumulator, kAccumulatorRefCountOffset, 1);
    ptr_SetRenderMode(accumulator, kShadowRenderMode);
    ptr_SetDepthPassIndex(accumulator, splitIndex);
    constructed = true;
    return accumulator;
}

bool EnsureScratchAccumulators(DirectionalSplitContext& ctx) noexcept
{
    if (!ValidSplitIndex(ctx.slotIndex)) {
        return false;
    }
    void* staticAccumulator = ConstructScratchAccumulator(
        s_staticAccumulatorStorage[ctx.slotIndex].bytes,
        s_staticAccumulatorConstructed[ctx.slotIndex],
        ctx.slotIndex);
    void* dynamicAccumulator = ConstructScratchAccumulator(
        s_dynamicAccumulatorStorage[ctx.slotIndex].bytes,
        s_dynamicAccumulatorConstructed[ctx.slotIndex],
        ctx.slotIndex);
    ctx.staticAccumulator = staticAccumulator;
    ctx.dynamicAccumulator = dynamicAccumulator;
    return staticAccumulator && dynamicAccumulator;
}

void ClearScratchAccumulator(void* accumulator) noexcept
{
    if (!accumulator) {
        return;
    }

    ptr_ClearActivePasses(accumulator, true);
    ptr_ClearRenderPasses(accumulator);
    ptr_SetShadowSceneNode(accumulator, nullptr);
    ptr_SetShadowLight(accumulator, nullptr);
}

void ConfigureScratchAccumulator(void* scratch, void* vanilla, std::uint32_t slotIndex) noexcept
{
    if (!scratch || !vanilla) {
        return;
    }

    ClearScratchAccumulator(scratch);
    const auto renderMode = ReadField<std::uint32_t>(vanilla, kAccumulatorRenderModeOffset, kShadowRenderMode);
    ptr_SetRenderMode(scratch, renderMode);

    auto* shadowSceneNode = ReadField<void*>(vanilla, kAccumulatorActiveShadowSceneNodeOffset);
    ptr_SetShadowSceneNode(scratch, shadowSceneNode);

    ptr_SetDepthPassIndex(scratch, slotIndex);

    // Copy the small render-flag/silhouette block, but leave owned containers
    // and renderer internals on the scratch accumulator's own storage.
    std::memcpy(
        static_cast<std::byte*>(scratch) + 0xB0,
        static_cast<std::byte*>(vanilla) + 0xB0,
        0x18);
}

bool PrepareScratchAccumulators(DirectionalSplitContext& ctx, void* vanillaAccumulator) noexcept
{
    if (!vanillaAccumulator || !EnsureScratchAccumulators(ctx)) {
        return false;
    }

    ConfigureScratchAccumulator(ctx.staticAccumulator, vanillaAccumulator, ctx.slotIndex);
    ConfigureScratchAccumulator(ctx.dynamicAccumulator, vanillaAccumulator, ctx.slotIndex);
    g_registrationStaticAccumulators[ctx.slotIndex].store(ctx.staticAccumulator, std::memory_order_release);
    g_registrationDynamicAccumulators[ctx.slotIndex].store(ctx.dynamicAccumulator, std::memory_order_release);
    return true;
}

bool PrepareDynamicScratchAccumulator(DirectionalSplitContext& ctx, void* vanillaAccumulator) noexcept
{
    if (!vanillaAccumulator ||
        !ValidSplitIndex(ctx.slotIndex)) {
        return false;
    }

    void* dynamicAccumulator = ConstructScratchAccumulator(
        s_dynamicAccumulatorStorage[ctx.slotIndex].bytes,
        s_dynamicAccumulatorConstructed[ctx.slotIndex],
        ctx.slotIndex);
    if (!dynamicAccumulator) {
        return false;
    }

    ctx.dynamicAccumulator = dynamicAccumulator;
    ConfigureScratchAccumulator(ctx.dynamicAccumulator, vanillaAccumulator, ctx.slotIndex);
    g_registrationStaticAccumulators[ctx.slotIndex].store(nullptr, std::memory_order_release);
    g_registrationDynamicAccumulators[ctx.slotIndex].store(ctx.dynamicAccumulator, std::memory_order_release);
    return true;
}

void ClearScratchAccumulators(DirectionalSplitContext& ctx) noexcept
{
    ClearScratchAccumulator(ctx.staticAccumulator);
    ClearScratchAccumulator(ctx.dynamicAccumulator);
}

void ClearAllScratchAccumulators() noexcept
{
    for (auto& ctx : s_directionalSplitContexts) {
        ClearScratchAccumulators(ctx);
    }
}

std::byte* ShadowMapDataForSlot(void* light, std::uint32_t slotIndex) noexcept
{
    if (!light) {
        return nullptr;
    }

    const auto count = ReadField<std::uint32_t>(light, kShadowMapDataCountOffset);
    if (slotIndex >= count) {
        return nullptr;
    }

    auto* base = ReadField<std::byte*>(light, kShadowMapDataArrayOffset);
    if (!base) {
        return nullptr;
    }
    return base + static_cast<std::size_t>(slotIndex) * kShadowMapDataStride;
}

Scope MakeScope(void* light, void* shadowMapData, LightKind kind) noexcept
{
    Scope scope;
    scope.shadowMapData = shadowMapData;
    scope.key.light = light;
    scope.key.kind = kind;
    scope.shadowMapDataIndex = ShadowMapDataIndex(light, shadowMapData);
    scope.camera = ReadField<void*>(shadowMapData, 0x40);
    scope.key.accumulator = ReadField<void*>(shadowMapData, kShadowMapAccumulatorOffset);
    scope.key.depthTarget = ReadField<std::uint32_t>(shadowMapData, kShadowMapDepthTargetOffset);
    scope.key.mapSlot = ReadField<std::uint32_t>(shadowMapData, kShadowMapMapSlotOffset);
    scope.key.cameraSig = ShadowCameraSignature(scope.camera, scope.key.mapSlot) ^
        (GameplayCameraTranslationSignature() + 0x9e3779b97f4a7c15ull);
    scope.key.dominantLightSig = ShadowLightDirectionSignature(light, scope.key.mapSlot);
    scope.key.activeDepthStencilView = ActiveShadowDSV(scope.key);
    scope.key.activeDepthTexture = ActiveShadowTextureIdentity(
        static_cast<REX::W32::ID3D11DepthStencilView*>(scope.key.activeDepthStencilView));
    scope.key.viewport.d0 = ReadField<std::uint32_t>(shadowMapData, 0xD0);
    scope.key.viewport.d4 = ReadField<std::uint32_t>(shadowMapData, 0xD4);
    scope.key.viewport.d8 = ReadField<std::uint32_t>(shadowMapData, 0xD8);
    scope.key.viewport.dc = ReadField<std::uint32_t>(shadowMapData, 0xDC);
    scope.cullingProcess = ReadField<void*>(shadowMapData, 0xE0);
    scope.inDeferredLights = ShadowUpgrade::IsInDeferredLighting();
    scope.start = std::chrono::steady_clock::now();
    return scope;
}

void PublishRegistrationTarget(
    const Scope& scope,
    DirectionalSplitContext& ctx,
    ShadowCachePass pass) noexcept
{
    if (!ValidSplitIndex(ctx.slotIndex)) {
        return;
    }
    if (pass == ShadowCachePass::BuildBoth ||
        pass == ShadowCachePass::DynamicRegister) {
        g_registrationStaticKeepCounts[ctx.slotIndex].store(0, std::memory_order_relaxed);
        g_registrationDynamicKeepCounts[ctx.slotIndex].store(0, std::memory_order_relaxed);
    }
    const bool buildBoth = pass == ShadowCachePass::BuildBoth;
    const bool dynamicOnly = pass == ShadowCachePass::DynamicRegister;
    tl_shadowCachePass = pass;
    g_registrationShadowCacheAccumulators[ctx.slotIndex].store(scope.key.accumulator, std::memory_order_release);
    g_registrationShadowCachePass.store(pass, std::memory_order_release);
    g_registrationShadowCacheActive.store(buildBoth || dynamicOnly, std::memory_order_release);
    g_registrationBuildBothBySplit[ctx.slotIndex].store(buildBoth, std::memory_order_release);
    g_registrationDynamicOnlyBySplit[ctx.slotIndex].store(dynamicOnly, std::memory_order_release);
    g_shadowCacheTargetMapSlot.store(scope.key.mapSlot, std::memory_order_release);
    g_shadowCacheTargetDepthTarget.store(scope.key.depthTarget, std::memory_order_release);
    g_shadowCacheTargetSplit.store(ctx.slotIndex, std::memory_order_release);
    g_shadowCacheTargetActive.store(true, std::memory_order_release);
}

void ClearRegistrationTarget() noexcept
{
    g_registrationShadowCachePass.store(ShadowCachePass::None, std::memory_order_release);
    g_registrationShadowCacheActive.store(false, std::memory_order_release);
    for (std::uint32_t i = 0; i < kMaxDirectionalCacheSplits; ++i) {
        g_registrationShadowCacheAccumulators[i].store(nullptr, std::memory_order_release);
        g_registrationStaticAccumulators[i].store(nullptr, std::memory_order_release);
        g_registrationDynamicAccumulators[i].store(nullptr, std::memory_order_release);
        g_registrationBuildBothBySplit[i].store(false, std::memory_order_release);
        g_registrationDynamicOnlyBySplit[i].store(false, std::memory_order_release);
        g_registrationStaticKeepCounts[i].store(0, std::memory_order_release);
        g_registrationDynamicKeepCounts[i].store(0, std::memory_order_release);
    }
    g_shadowCacheTargetActive.store(false, std::memory_order_release);
    g_shadowCacheTargetMapSlot.store((std::numeric_limits<std::uint32_t>::max)(), std::memory_order_release);
    g_shadowCacheTargetDepthTarget.store((std::numeric_limits<std::uint32_t>::max)(), std::memory_order_release);
    g_shadowCacheTargetSplit.store(kInvalidSplitIndex, std::memory_order_release);
    tl_shadowCacheRenderSplit = kInvalidSplitIndex;
    tl_lastRegistrationSplit = kInvalidSplitIndex;
}

bool IsClose(std::uint32_t a, std::uint32_t b, std::uint32_t tolerance) noexcept
{
    return a > b ? (a - b) <= tolerance : (b - a) <= tolerance;
}

bool ViewportClose(const ViewportSig& a, const ViewportSig& b) noexcept
{
    constexpr std::uint32_t kViewportTolerance = 4;
    return IsClose(a.d0, b.d0, kViewportTolerance) &&
           IsClose(a.d4, b.d4, kViewportTolerance) &&
           IsClose(a.d8, b.d8, kViewportTolerance) &&
           IsClose(a.dc, b.dc, kViewportTolerance);
}

bool ViewportStable(const Key& cached, const Key& current) noexcept
{
    if (cached.mapSlot == 0 || current.mapSlot == 0) {
        return cached.viewport == current.viewport;
    }
    return ViewportClose(cached.viewport, current.viewport);
}

bool CacheKeyStable(const Key& cached, const Key& current) noexcept
{
    return cached.cameraSig == current.cameraSig &&
           cached.dominantLightSig == current.dominantLightSig &&
           cached.depthTarget == current.depthTarget &&
           cached.mapSlot == current.mapSlot &&
           cached.kind == current.kind &&
           ViewportStable(cached, current);
}

void MaybeLogShadowCache(
    const Scope& scope,
    ShadowCacheState& cache,
    const StaticDepthCache& depthCache,
    const char* decision,
    bool cacheEligible,
    bool cacheHit,
    bool preCacheValid,
    bool preDepthValid,
    bool preStableKey)
{
    if constexpr (!kShadowCacheLogEnabled) {
        return;
    }

    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        return;
    }

    const bool hasActivity =
        cache.eligible != 0 ||
        cache.hits != 0 ||
        cache.misses != 0 ||
        cache.staticBuilds != 0 ||
        cache.captured != 0 ||
        cache.emptyStaticBuilds != 0 ||
        cache.restoreFailures != 0 ||
        cache.shadowMapOrMaskHookCalls != 0 ||
        cache.invalidations != 0;
    if (!hasActivity) {
        return;
    }

    const ShadowCachePass registrationPass =
        g_registrationShadowCachePass.load(std::memory_order_acquire);
    const bool registrationActive =
        g_registrationShadowCacheActive.load(std::memory_order_acquire);
    const auto cachedKeyHash = static_cast<std::uint64_t>(KeyHash{}(cache.key));
    const auto currentKeyHash = static_cast<std::uint64_t>(KeyHash{}(scope.key));
    const bool keyLightEq = cache.key.light == scope.key.light;
    const bool keyDSVEq = cache.key.activeDepthStencilView == scope.key.activeDepthStencilView;
    const bool keyTexEq = cache.key.activeDepthTexture == scope.key.activeDepthTexture;
    const bool keyCameraEq = cache.key.cameraSig == scope.key.cameraSig;
    const bool keySunEq = cache.key.dominantLightSig == scope.key.dominantLightSig;
    const bool keyDepthEq = cache.key.depthTarget == scope.key.depthTarget;
    const bool keySlotEq = cache.key.mapSlot == scope.key.mapSlot;
    const bool keyKindEq = cache.key.kind == scope.key.kind;
    const bool keyViewportEq = ViewportStable(cache.key, scope.key);

    REX::INFO(
        "ShadowCache[directional idx={} mapSlot={} targetSplit={}]: decision={} eligibleNow={} hitNow={} "
        "pre(valid/depth/stable)={}/{}/{} "
        "post(valid/depth/stableHits/skips)={}/{}/{}/{} "
        "key(cur/cache)={:#x}/{:#x} keyEq(l/dsv/tex/cam/sun/depth/slot/kind/vp)={}/{}/{}/{}/{}/{}/{}/{}/{} viewport=({},{},{},{}) "
        "dsv(validMask={:#x}) pass={}/{} "
        "eligible={} hits={} misses={} staticBuilds={} captures={} "
        "emptyStatic={} restoreFailures={} staticKeep={} staticSkip={} overlayKeep={} overlaySkip={} "
        "regSM={}/{} smPass(n/p/b/r/s/d={}/{}/{}/{}/{}/{}, active={}/{}/{}/{}/{}/{}) "
        "updates={} skips={} invalidations={}",
        scope.shadowMapDataIndex,
        scope.key.mapSlot,
        scope.shadowMapDataIndex,
        decision ? decision : "?",
        cacheEligible,
        cacheHit,
        preCacheValid,
        preDepthValid,
        preStableKey,
        cache.valid,
        depthCache.valid,
        cache.stableKeyHits,
        cache.skipsSinceUpdate,
        currentKeyHash,
        cachedKeyHash,
        keyLightEq,
        keyDSVEq,
        keyTexEq,
        keyCameraEq,
        keySunEq,
        keyDepthEq,
        keySlotEq,
        keyKindEq,
        keyViewportEq,
        scope.key.viewport.d0,
        scope.key.viewport.d4,
        scope.key.viewport.d8,
        scope.key.viewport.dc,
        depthCache.validSubresources,
        PassShortName(registrationPass),
        registrationActive,
        cache.eligible,
        cache.hits,
        cache.misses,
        cache.staticBuilds,
        cache.captured,
        cache.emptyStaticBuilds,
        cache.restoreFailures,
        cache.staticKept,
        cache.staticSkipped,
        cache.overlayKept,
        cache.overlaySkipped,
        cache.shadowMapOrMaskHookActiveCalls,
        cache.shadowMapOrMaskHookCalls,
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::None)],
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::Passthrough)],
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::BuildBoth)],
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::DynamicRegister)],
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::StaticRender)],
        cache.shadowMapOrMaskHookCallsByPass[PassIndex(ShadowCachePass::DynamicRender)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::None)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::Passthrough)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::BuildBoth)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::DynamicRegister)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::StaticRender)],
        cache.shadowMapOrMaskHookActiveCallsByPass[PassIndex(ShadowCachePass::DynamicRender)],
        cache.updates,
        cache.skips,
        cache.invalidations);
    cache.eligible = 0;
    cache.updates = 0;
    cache.skips = 0;
    cache.invalidations = 0;
    cache.hits = 0;
    cache.misses = 0;
    cache.staticBuilds = 0;
    cache.restoreFailures = 0;
    cache.captured = 0;
    cache.emptyStaticBuilds = 0;
    cache.staticKept = 0;
    cache.staticSkipped = 0;
    cache.overlayKept = 0;
    cache.overlaySkipped = 0;
    cache.shadowMapOrMaskHookCalls = 0;
    cache.shadowMapOrMaskHookActiveCalls = 0;
    cache.shadowMapOrMaskHookCallsByPass = {};
    cache.shadowMapOrMaskHookActiveCallsByPass = {};
}

void MaybeLogDirectionalAccumulation(std::uint32_t liveCount)
{
    if constexpr (!kShadowCacheLogEnabled) {
        return;
    }

    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - s_lastCacheLogTime).count();
    if (secs < kLogIntervalSecs) {
        return;
    }
    s_lastCacheLogTime = now;

    std::ostringstream splits;
    for (std::uint32_t i = 0; i < liveCount; ++i) {
        const auto& ctx = ContextForSplit(i);
        const auto& cache = CacheForSplit(i);
        const auto& depth = DepthCacheForSplit(i);
        if (i != 0) {
            splits << ' ';
        }
        splits << "[idx=" << i
               << " mapSlot=" << ctx.key.mapSlot
               << " eligible=" << ctx.eligible
               << " hit=" << ctx.cacheHit
               << " build=" << ctx.buildBoth
               << " empty=" << ctx.splitEmpty
               << " valid/depth=" << cache.valid << '/' << depth.valid
               << " routedS/D=" << ctx.buildStaticRouted << '/'
               << g_registrationDynamicKeepCounts[i].load(std::memory_order_relaxed)
               << ']';
    }

    REX::INFO("ShadowCacheAccum[directional liveSplits={} capped={}]: {}",
              liveCount,
              kMaxDirectionalCacheSplits,
              splits.str());
}

bool IsDirectionalSplitCacheEligible(const Scope& scope) noexcept
{
    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
        scope.key.kind != LightKind::Directional ||
        !ValidSplitIndex(scope.shadowMapDataIndex) ||
        scope.key.mapSlot < kFirstCacheableDirectionalMapSlot ||
        !scope.key.light ||
        !scope.key.accumulator) {
        return false;
    }

    return true;
}

bool DirectionalSplitCacheHit(const Scope& scope, ShadowCacheState& cache, StaticDepthCache& depthCache) noexcept
{
    const bool stableKey = CacheKeyStable(cache.key, scope.key);
    if (!cache.valid ||
        !depthCache.valid) {
        cache.stableKeyHits = 0;
        return false;
    }

    if (!stableKey) {
        ++cache.invalidations;
        cache.valid = false;
        cache.skipsSinceUpdate = 0;
        cache.stableKeyHits = 0;
        ReleaseStaticDepthCache(depthCache);
        return false;
    }

    ++cache.skipsSinceUpdate;
    ++cache.stableKeyHits;
    ++cache.skips;
    ++cache.hits;
    return true;
}

bool DirectionalSplitStaticBuildReady(const Scope& scope, ShadowCacheState& cache, const StaticDepthCache& depthCache) noexcept
{
    if (cache.valid &&
        depthCache.valid &&
        CacheKeyStable(cache.key, scope.key)) {
        cache.pendingBuildKey = {};
        cache.pendingBuildKeyHits = 0;
        return true;
    }

    if (!CacheKeyStable(cache.pendingBuildKey, scope.key)) {
        cache.pendingBuildKey = scope.key;
        cache.pendingBuildKeyHits = 1;
        return false;
    }

    if (cache.pendingBuildKeyHits < 3) {
        ++cache.pendingBuildKeyHits;
    }
    return cache.pendingBuildKeyHits >= 2;
}

void MarkDirectionalSplitCacheUpdated(const Scope& scope) noexcept
{
    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
        scope.key.kind != LightKind::Directional ||
        !ValidSplitIndex(scope.shadowMapDataIndex)) {
        return;
    }

    ++CacheForSplit(scope.shadowMapDataIndex).updates;
}

void MaybeLog()
{
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - s_lastLogTime).count();
    if (secs < kLogIntervalSecs || s_total.calls == 0) return;
    s_lastLogTime = now;

    REX::INFO(
        "ShadowTelemetry[RenderShadowMap]: calls={} over {:.2f}s ({:.1f}/s) "
        "totalMs/s={:.2f} avgUs={:.1f} maxUs={:.1f} bsDraws/call={:.1f} "
        "cmdBuf/call={:.1f} d3dDraws/call={:.1f} renderSceneMs/s={:.2f} inDeferredLights={}/{}",
        s_total.calls, secs, static_cast<double>(s_total.calls) / secs,
        (s_total.totalNs / 1'000'000.0) / secs,
        (s_total.totalNs / 1000.0) / static_cast<double>(s_total.calls),
        s_total.maxNs / 1000.0,
        static_cast<double>(s_total.bsDraws) / static_cast<double>(s_total.calls),
        static_cast<double>(s_total.cmdBufDraws) / static_cast<double>(s_total.calls),
        static_cast<double>(s_total.d3dDraws) / static_cast<double>(s_total.calls),
        (s_total.renderSceneNs / 1'000'000.0) / secs,
        s_total.deferredLightsCalls, s_total.calls);

    std::vector<std::pair<Key, Aggregate>> rows;
    rows.reserve(s_byKey.size());
    for (const auto& item : s_byKey) {
        rows.push_back(item);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.second.totalNs > b.second.totalNs;
    });

    const std::size_t n = rows.size() < kMaxLoggedKeys ? rows.size() : kMaxLoggedKeys;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& [key, b] = rows[i];
        REX::INFO(
            "  Shadow[{}] light={} accum={} dsv={} tex={} depthTarget={} mapSlot={} viewport=({},{},{},{}) "
            "calls={} totalMs/s={:.2f} avgUs={:.1f} d3dDraws/call={:.1f} bsDraws/call={:.1f}",
            LightKindName(key.kind),
            key.light,
            key.accumulator,
            key.activeDepthStencilView,
            key.activeDepthTexture,
            key.depthTarget,
            key.mapSlot,
            key.viewport.d0, key.viewport.d4, key.viewport.d8, key.viewport.dc,
            b.calls,
            (b.totalNs / 1'000'000.0) / secs,
            (b.totalNs / 1000.0) / static_cast<double>(b.calls),
            static_cast<double>(b.d3dDraws) / static_cast<double>(b.calls),
            static_cast<double>(b.bsDraws) / static_cast<double>(b.calls));
    }

    REX::INFO(
        "ShadowTelemetry[Work]: replay calls={} ms/s={:.2f} d3d/call={:.1f}; "
        "immediate calls={} ms/s={:.2f} d3d/call={:.1f}; buildCB calls={} ms/s={:.2f}",
        s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].calls,
        (s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].totalNs / 1'000'000.0) / secs,
        s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].calls
            ? static_cast<double>(s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].d3dDraws) /
                  static_cast<double>(s_workTotals[WorkIndex(WorkKind::CommandBufferReplay)].calls)
            : 0.0,
        s_workTotals[WorkIndex(WorkKind::ImmediatePass)].calls,
        (s_workTotals[WorkIndex(WorkKind::ImmediatePass)].totalNs / 1'000'000.0) / secs,
        s_workTotals[WorkIndex(WorkKind::ImmediatePass)].calls
            ? static_cast<double>(s_workTotals[WorkIndex(WorkKind::ImmediatePass)].d3dDraws) /
                  static_cast<double>(s_workTotals[WorkIndex(WorkKind::ImmediatePass)].calls)
            : 0.0,
        s_workTotals[WorkIndex(WorkKind::BuildCommandBuffer)].calls,
        (s_workTotals[WorkIndex(WorkKind::BuildCommandBuffer)].totalNs / 1'000'000.0) / secs);

    std::vector<std::pair<WorkKey, WorkAggregate>> workRows;
    workRows.reserve(s_workByKey.size());
    for (const auto& item : s_workByKey) {
        workRows.push_back(item);
    }
    std::sort(workRows.begin(), workRows.end(), [](const auto& a, const auto& b) {
        return a.second.totalNs > b.second.totalNs;
    });

    const std::size_t workN = workRows.size() < kMaxLoggedWorkKeys ? workRows.size() : kMaxLoggedWorkKeys;
    for (std::size_t i = 0; i < workN; ++i) {
        const auto& [key, b] = workRows[i];
        REX::INFO(
            "  ShadowWork[{}] light={} mapSlot={} group={} sub={} chain={} rec={} recDraws={} nonZeroDraws={} maxDraw={} recSrv={} maxSrv={} "
            "geom={} shader={} prop={} tech=0x{:08X} cbData={} firstRec={} "
            "calls={} totalMs/s={:.2f} avgUs={:.1f} d3d/call={:.1f}",
            WorkKindName(key.kind),
            key.shadow.light,
            key.shadow.mapSlot,
            key.passGroupIdx,
            key.subIdx,
            key.chainLen,
            key.cbRecordCount,
            key.cbDrawCount,
            key.cbNonZeroDrawCount,
            key.cbMaxDrawCount,
            key.cbSrvRecordCount,
            key.cbMaxSrvRecordCount,
            key.geometry,
            key.shader,
            key.shaderProperty,
            key.techniqueID,
            key.cbData,
            key.commandBuffer,
            b.calls,
            (b.totalNs / 1'000'000.0) / secs,
            (b.totalNs / 1000.0) / static_cast<double>(b.calls),
            b.calls ? static_cast<double>(b.d3dDraws) / static_cast<double>(b.calls) : 0.0);
    }

    ResetWindow();
}

void HookedRenderScene(void* camera, void* accumulator, bool flag)
{
    if (tl_scopes.empty()) {
        s_origRenderScene(camera, accumulator, flag);
        return;
    }

    const auto t0 = std::chrono::steady_clock::now();
    s_origRenderScene(camera, accumulator, flag);
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    if (g_mode.load(std::memory_order_relaxed) == Mode::On) {
        auto& scope = tl_scopes.back();
        ++scope.renderSceneCalls;
        scope.renderSceneNs += ns;
    }
}

void SetRegistrationShadowCachePass(ShadowCachePass pass) noexcept
{
    const bool active = pass == ShadowCachePass::BuildBoth;
    tl_shadowCachePass = pass;
    g_registrationShadowCachePass.store(pass, std::memory_order_release);
    g_registrationShadowCacheActive.store(active, std::memory_order_release);
}

void IncrementRegistrationCounter(std::uint64_t& counter) noexcept
{
    if constexpr (!kShadowCacheLogEnabled) {
        return;
    }
    std::atomic_ref<std::uint64_t>(counter).fetch_add(1, std::memory_order_relaxed);
}

void BeginShadowCachePass(DirectionalSplitContext* ctx, ShadowCachePass pass) noexcept
{
    if (ctx) {
        ctx->phase = pass;
        tl_shadowCacheRenderSplit = ctx->slotIndex;
    } else {
        tl_shadowCacheRenderSplit = kInvalidSplitIndex;
    }
    SetRegistrationShadowCachePass(pass);
    if (pass == ShadowCachePass::Passthrough || pass == ShadowCachePass::None) {
        g_registrationShadowCacheActive.store(false, std::memory_order_release);
    }
    g_shadowCacheTargetActive.store(pass == ShadowCachePass::StaticRender ||
                                        pass == ShadowCachePass::DynamicRender,
                                    std::memory_order_release);
    g_shadowCacheTargetSplit.store(ctx ? ctx->slotIndex : kInvalidSplitIndex, std::memory_order_release);
    if (ctx) {
        g_shadowCacheTargetMapSlot.store(ctx->key.mapSlot, std::memory_order_release);
        g_shadowCacheTargetDepthTarget.store(ctx->key.depthTarget, std::memory_order_release);
    }
}

bool RunOriginalRenderShadowMap(void* light, void* shadowMapData)
{
    if (!s_origRenderShadowMap || !light || !shadowMapData) {
        return false;
    }

    s_origRenderShadowMap(light, shadowMapData);
    return true;
}

class ScopedShadowMapAccumulatorSwap
{
public:
    ScopedShadowMapAccumulatorSwap(void* shadowMapData, void* replacement) :
        shadowMapData_(shadowMapData),
        original_(ReadField<void*>(shadowMapData, kShadowMapAccumulatorOffset))
    {
        if (!shadowMapData_ || !replacement) {
            return;
        }

        WriteField<void*>(shadowMapData_, kShadowMapAccumulatorOffset, replacement);
        active_ = true;
    }

    ~ScopedShadowMapAccumulatorSwap()
    {
        if (active_) {
            WriteField<void*>(shadowMapData_, kShadowMapAccumulatorOffset, original_);
        }
    }

    bool Active() const noexcept { return active_; }

    ScopedShadowMapAccumulatorSwap(const ScopedShadowMapAccumulatorSwap&) = delete;
    ScopedShadowMapAccumulatorSwap& operator=(const ScopedShadowMapAccumulatorSwap&) = delete;

private:
    void* shadowMapData_ = nullptr;
    void* original_ = nullptr;
    bool active_ = false;
};

void ResetDirectionalSplitFrameContexts() noexcept
{
    for (std::uint32_t i = 0; i < kMaxDirectionalCacheSplits; ++i) {
        void* staticAccumulator = s_directionalSplitContexts[i].staticAccumulator;
        void* dynamicAccumulator = s_directionalSplitContexts[i].dynamicAccumulator;
        const std::uint32_t passthroughCooldown = s_directionalSplitContexts[i].passthroughCooldown;
        s_directionalSplitContexts[i] = {};
        s_directionalSplitContexts[i].staticAccumulator = staticAccumulator;
        s_directionalSplitContexts[i].dynamicAccumulator = dynamicAccumulator;
        s_directionalSplitContexts[i].slotIndex = i;
        s_directionalSplitContexts[i].passthroughCooldown = passthroughCooldown > 0 ? passthroughCooldown - 1 : 0;
    }
}

bool ContextMatchesScope(const DirectionalSplitContext& ctx, const Scope& scope) noexcept
{
    return ctx.sunLight == scope.key.light &&
           ctx.shadowMapData == scope.shadowMapData &&
           ctx.vanillaAccumulator == scope.key.accumulator &&
           ctx.key.depthTarget == scope.key.depthTarget &&
           ctx.key.mapSlot == scope.key.mapSlot &&
           ctx.key.kind == scope.key.kind &&
           CacheKeyStable(ctx.key, scope.key);
}

REX::W32::ID3D11DepthStencilView* ResolveShadowDSV(const DirectionalSplitContext& ctx, const Scope& scope) noexcept
{
    auto* active = ActiveShadowDSVFromRendererState(scope.key);
    if (active) {
        return active;
    }

    if (ctx.activeDepthStencilView &&
        DSVSliceMatchesMapSlot(ctx.activeDepthStencilView, scope.key.mapSlot)) {
        return ctx.activeDepthStencilView;
    }
    return nullptr;
}

bool ShadowPassDSVMatches(const DirectionalSplitContext& ctx, REX::W32::ID3D11DepthStencilView* dsv) noexcept
{
    if (!dsv) {
        return false;
    }

    if (auto* active = ActiveShadowDSVFromRendererState(ctx.key)) {
        return dsv == active;
    }

    return DSVSliceMatchesMapSlot(dsv, ctx.key.mapSlot);
}

void MaybeLogShadowCacheClearReject(
    const char* reason,
    std::atomic_uint64_t& counter,
    REX::W32::ID3D11DepthStencilView* dsv,
    std::uint32_t clearFlags,
    float depth,
    std::uint8_t stencil) noexcept
{
    if constexpr (!kShadowCacheLogEnabled) {
        return;
    }

    const std::uint64_t count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 16 && (count % 128) != 0) {
        return;
    }

    const auto split = tl_shadowCacheRenderSplit;
    const bool validSplit = ValidSplitIndex(split);
    const DirectionalSplitContext* ctx = validSplit ? &ContextForSplit(split) : nullptr;
    const Key* key = ctx ? &ctx->key : nullptr;
    auto* expected = key ? ActiveShadowDSV(*key) : nullptr;
    void* incomingTex = ActiveShadowTextureIdentity(dsv);
    void* expectedTex = ActiveShadowTextureIdentity(expected);
    const bool incomingSlotMatches = key && DSVSliceMatchesMapSlot(dsv, key->mapSlot);
    const bool expectedSlotMatches = key && DSVSliceMatchesMapSlot(expected, key->mapSlot);

    REX::WARN(
        "ShadowCacheClearReject[{} #{}]: pass={} targetActive={} split={} validSplit={} "
        "incomingDSV={} incomingTex={} incomingSlotMatch={} expectedDSV={} expectedTex={} expectedSlotMatch={} "
        "keyDSV={} keyTex={} depthTarget={} mapSlot={} targetDepth={} targetSlot={} flags={:#x} depth={} stencil={}",
        reason ? reason : "?",
        count,
        PassShortName(tl_shadowCachePass),
        g_shadowCacheTargetActive.load(std::memory_order_acquire),
        split,
        validSplit,
        static_cast<void*>(dsv),
        incomingTex,
        incomingSlotMatches,
        static_cast<void*>(expected),
        expectedTex,
        expectedSlotMatches,
        key ? key->activeDepthStencilView : nullptr,
        key ? key->activeDepthTexture : nullptr,
        key ? key->depthTarget : 0,
        key ? key->mapSlot : 0,
        g_shadowCacheTargetDepthTarget.load(std::memory_order_acquire),
        g_shadowCacheTargetMapSlot.load(std::memory_order_acquire),
        clearFlags,
        depth,
        stencil);
}

void MarkStaticCachePublished(DirectionalSplitContext& ctx, ShadowCacheState& cache, const Scope& scope) noexcept
{
    cache.valid = true;
    cache.key = scope.key;
    if (ctx.activeDepthStencilView) {
        cache.key.activeDepthStencilView = ctx.activeDepthStencilView;
        cache.key.activeDepthTexture = ActiveShadowTextureIdentity(ctx.activeDepthStencilView);
    }
    cache.skipsSinceUpdate = 0;
    cache.stableKeyHits = 0;
    cache.pendingBuildKey = {};
    cache.pendingBuildKeyHits = 0;
    ctx.cacheHit = false;
}

bool CaptureAndPublishStaticCache(DirectionalSplitContext& ctx)
{
    if (!ValidSplitIndex(ctx.slotIndex)) {
        return false;
    }

    auto& cache = CacheForSplit(ctx.slotIndex);
    auto& depthCache = DepthCacheForSplit(ctx.slotIndex);
    auto* dsv = ctx.activeDepthStencilView &&
            DSVSliceMatchesMapSlot(ctx.activeDepthStencilView, ctx.key.mapSlot) ?
        ctx.activeDepthStencilView :
        ActiveShadowDSVFromRendererState(ctx.key);

    ctx.staticCaptureAttempted = true;
    if (!CaptureStaticCacheFromBuildDSV(ctx.slotIndex, dsv)) {
        cache.valid = false;
        ReleaseStaticDepthCache(depthCache);
        return false;
    }

    ctx.staticCaptureSucceeded = true;
    cache.valid = true;
    cache.key = ctx.key;
    if (dsv) {
        cache.key.activeDepthStencilView = dsv;
        cache.key.activeDepthTexture = ActiveShadowTextureIdentity(dsv);
    }
    cache.skipsSinceUpdate = 0;
    cache.stableKeyHits = 0;
    cache.pendingBuildKey = {};
    cache.pendingBuildKeyHits = 0;
    ctx.cacheHit = false;
    ctx.passthroughCooldown = 0;
    return true;
}

void InvalidateStaticCache(std::uint32_t splitIndex) noexcept
{
    if (!ValidSplitIndex(splitIndex)) {
        return;
    }
    auto& cache = CacheForSplit(splitIndex);
    ++cache.invalidations;
    cache.valid = false;
    cache.skipsSinceUpdate = 0;
    cache.stableKeyHits = 0;
    ReleaseStaticDepthCache(DepthCacheForSplit(splitIndex));
}

bool RunRenderShadowMapWithAccumulator(
    DirectionalSplitContext& ctx,
    void* light,
    void* shadowMapData,
    void* accumulator,
    ShadowCachePass pass)
{
    ScopedShadowMapAccumulatorSwap swap(shadowMapData, accumulator);
    if (!swap.Active()) {
        return false;
    }

    BeginShadowCachePass(&ctx, pass);
    const bool ok = RunOriginalRenderShadowMap(light, shadowMapData);
    BeginShadowCachePass(&ctx, ShadowCachePass::None);
    return ok;
}

void RunPassthroughShadowMap(void* light, void* shadowMapData)
{
    BeginShadowCachePass(nullptr, ShadowCachePass::Passthrough);
    RunOriginalRenderShadowMap(light, shadowMapData);
    BeginShadowCachePass(nullptr, ShadowCachePass::None);
}

bool RunRoutedPassthroughShadowMap(DirectionalSplitContext& ctx, void* light, void* shadowMapData)
{
    if (!ctx.dynamicAccumulator) {
        return false;
    }

    ctx.restoreFailedThisFrame = false;
    ctx.activeDepthStencilView = nullptr;
    return RunRenderShadowMapWithAccumulator(
        ctx,
        light,
        shadowMapData,
        ctx.dynamicAccumulator,
        ShadowCachePass::Passthrough);
}

bool RunScratchFullFallbackShadowMap(DirectionalSplitContext& ctx, void* light, void* shadowMapData)
{
    if (!ctx.staticAccumulator || !ctx.dynamicAccumulator) {
        return false;
    }

    ctx.fullFallbackThisFrame = true;
    ctx.restoreFailedThisFrame = false;
    ctx.dynamicRestoreAttempted = false;
    ctx.dynamicRestoreSucceeded = false;
    ctx.activeDepthStencilView = nullptr;

    if (!RunRenderShadowMapWithAccumulator(
            ctx,
            light,
            shadowMapData,
            ctx.staticAccumulator,
            ShadowCachePass::StaticRender)) {
        return false;
    }

    ctx.staticRenderedThisFrame = true;
    ctx.staticCaptureSucceeded = false;
    CaptureAndPublishStaticCache(ctx);

    return RunRenderShadowMapWithAccumulator(
        ctx,
        light,
        shadowMapData,
        ctx.dynamicAccumulator,
        ShadowCachePass::DynamicRender);
}

bool RunSplitBuildShadowMap(DirectionalSplitContext& ctx, void* light, void* shadowMapData, const Scope& scope)
{
    auto& cache = CacheForSplit(ctx.slotIndex);
    auto& depthCache = DepthCacheForSplit(ctx.slotIndex);
    if (!ctx.staticAccumulator || !ctx.dynamicAccumulator) {
        InvalidateStaticCache(ctx.slotIndex);
        return false;
    }

    ctx.restoreFailedThisFrame = false;
    ctx.staticRenderedThisFrame = false;
    ctx.staticCaptureSucceeded = false;
    ctx.staticCaptureAttempted = false;
    ctx.dynamicRestoreAttempted = false;
    ctx.dynamicRestoreSucceeded = false;
    ctx.activeDepthStencilView = nullptr;

    if (!RunRenderShadowMapWithAccumulator(
            ctx,
            light,
            shadowMapData,
            ctx.staticAccumulator,
            ShadowCachePass::StaticRender)) {
        InvalidateStaticCache(ctx.slotIndex);
        return false;
    }

    ctx.staticRenderedThisFrame = true;
    auto* staticDSV = ResolveShadowDSV(ctx, scope);

    const std::uint32_t staticCount = (std::max)(
        ctx.buildStaticRouted,
        g_registrationStaticKeepCounts[ctx.slotIndex].load(std::memory_order_acquire));
    const bool hasStaticRegistrations = staticCount != 0;
    if (!hasStaticRegistrations) {
        ++cache.emptyStaticBuilds;
    }

    ctx.staticCaptureAttempted = hasStaticRegistrations;
    if (hasStaticRegistrations &&
        CaptureStaticCacheFromBuildDSV(ctx.slotIndex, staticDSV)) {
        ctx.staticCaptureSucceeded = true;
        MarkStaticCachePublished(ctx, cache, scope);
    } else {
        cache.valid = false;
        ReleaseStaticDepthCache(depthCache);
    }

    if (!RunRenderShadowMapWithAccumulator(
            ctx,
            light,
            shadowMapData,
            ctx.dynamicAccumulator,
            ShadowCachePass::DynamicRender)) {
        InvalidateStaticCache(ctx.slotIndex);
        return false;
    }

    if (ctx.restoreFailedThisFrame) {
        InvalidateStaticCache(ctx.slotIndex);
        return false;
    }
    return true;
}

bool RunCachedDynamicShadowMap(DirectionalSplitContext& ctx, void* light, void* shadowMapData, const Scope&)
{
    if (!DepthCacheForSplit(ctx.slotIndex).valid || !ctx.dynamicAccumulator) {
        InvalidateStaticCache(ctx.slotIndex);
        return false;
    }

    ctx.restoreFailedThisFrame = false;
    ctx.fullFallbackThisFrame = false;
    ctx.staticRenderedThisFrame = false;
    ctx.staticCaptureSucceeded = false;
    ctx.dynamicRestoreAttempted = false;
    ctx.dynamicRestoreSucceeded = false;
    ctx.activeDepthStencilView = nullptr;
    if (!RunRenderShadowMapWithAccumulator(
            ctx,
            light,
            shadowMapData,
            ctx.dynamicAccumulator,
            ShadowCachePass::DynamicRender)) {
        InvalidateStaticCache(ctx.slotIndex);
        return false;
    }

    if (ctx.restoreFailedThisFrame) {
        InvalidateStaticCache(ctx.slotIndex);
        return RunScratchFullFallbackShadowMap(ctx, light, shadowMapData);
    }
    if (!ctx.dynamicRestoreSucceeded) {
        if constexpr (kShadowCacheLogEnabled) {
            REX::WARN(
                "ShadowCacheRestoreMissed[split={} mapSlot={}]: attempted={} activeDSV={} expectedDSV={} keyDSV={} keyTex={} cacheTex={} validMask={:#x} cacheValid={} phase={}",
                ctx.slotIndex,
                ctx.key.mapSlot,
                ctx.dynamicRestoreAttempted,
                static_cast<void*>(ctx.activeDepthStencilView),
                static_cast<void*>(ActiveShadowDSV(ctx.key)),
                ctx.key.activeDepthStencilView,
                ctx.key.activeDepthTexture,
                static_cast<void*>(DepthCacheForSplit(ctx.slotIndex).texture),
                DepthCacheForSplit(ctx.slotIndex).validSubresources,
                DepthCacheForSplit(ctx.slotIndex).valid,
                PassShortName(CurrentResolvedRegistrationPass()));
        }
        ++CacheForSplit(ctx.slotIndex).restoreFailures;
        InvalidateStaticCache(ctx.slotIndex);
        ctx.passthroughCooldown = 8;
        return RunScratchFullFallbackShadowMap(ctx, light, shadowMapData);
    }
    return true;
}

void HookedAccumulateFromLists(void* light, void* cullingGroup)
{
    if (!s_origAccumulateFromLists) {
        return;
    }

    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        s_origAccumulateFromLists(light, cullingGroup);
        return;
    }

    ResetDirectionalSplitFrameContexts();
    ClearRegistrationTarget();

    const auto liveCount = (std::min<std::uint32_t>)(
        ReadField<std::uint32_t>(light, kShadowMapDataCountOffset),
        kMaxDirectionalCacheSplits);
    for (std::uint32_t splitIndex = 0; splitIndex < liveCount; ++splitIndex) {
        auto* shadowMapData = ShadowMapDataForSlot(light, splitIndex);
        if (!shadowMapData) {
            continue;
        }

        Scope scope = MakeScope(light, shadowMapData, LightKind::Directional);
        auto& cache = CacheForSplit(splitIndex);
        auto& depthCache = DepthCacheForSplit(splitIndex);
        auto& ctx = ContextForSplit(splitIndex);
        const bool cacheEligible = IsDirectionalSplitCacheEligible(scope);
        bool cacheHit = false;
        bool buildReady = false;

        if (!cacheEligible) {
            continue;
        }

        ++cache.eligible;
        cacheHit = DirectionalSplitCacheHit(scope, cache, depthCache);
        buildReady = !cacheHit && DirectionalSplitStaticBuildReady(scope, cache, depthCache);

        ctx.sunLight = light;
        ctx.shadowMapData = shadowMapData;
        ctx.vanillaAccumulator = scope.key.accumulator;
        ctx.key = scope.key;
        ctx.eligible = true;
        ctx.cacheHit = cacheHit;

        if (ctx.passthroughCooldown != 0) {
            ctx.cacheHit = false;
            cacheHit = false;
        } else if (cacheHit) {
            if (PrepareDynamicScratchAccumulator(ctx, scope.key.accumulator)) {
                ctx.hitRouted = true;
                PublishRegistrationTarget(scope, ctx, ShadowCachePass::DynamicRegister);
            } else {
                ctx.cacheHit = false;
                ctx.splitFailed = true;
                InvalidateStaticCache(splitIndex);
            }
        } else if (buildReady) {
            ++cache.misses;
            ++cache.staticBuilds;
            if (PrepareScratchAccumulators(ctx, scope.key.accumulator)) {
                ctx.buildBoth = true;
                PublishRegistrationTarget(scope, ctx, ShadowCachePass::BuildBoth);
            } else {
                ctx.splitFailed = true;
                InvalidateStaticCache(splitIndex);
            }
        } else {
            ++cache.misses;
        }
    }

    s_origAccumulateFromLists(light, cullingGroup);

    for (std::uint32_t splitIndex = 0; splitIndex < liveCount; ++splitIndex) {
        auto& ctx = ContextForSplit(splitIndex);
        if (!ctx.buildBoth && !ctx.hitRouted) {
            continue;
        }
        const auto staticRouted = g_registrationStaticKeepCounts[splitIndex].load(std::memory_order_acquire);
        ctx.buildStaticRouted = staticRouted;
        if (ctx.buildBoth && staticRouted == 0) {
            ++CacheForSplit(splitIndex).emptyStaticBuilds;
            ctx.buildBoth = false;
            ctx.splitEmpty = true;
            auto& cache = CacheForSplit(splitIndex);
            cache.valid = false;
            cache.skipsSinceUpdate = 0;
            cache.stableKeyHits = 0;
            ReleaseStaticDepthCache(DepthCacheForSplit(splitIndex));
            ClearScratchAccumulator(ctx.staticAccumulator);
        }
    }
    MaybeLogDirectionalAccumulation(liveCount);
    ClearRegistrationTarget();
}

void HookedRenderShadowMap(void* light, void* shadowMapData)
{
    const bool telemetryOn = g_mode.load(std::memory_order_relaxed) == Mode::On;
    if (!telemetryOn && !SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        s_origRenderShadowMap(light, shadowMapData);
        return;
    }

    Scope scope = MakeScope(light, shadowMapData, ClassifyCaller(_ReturnAddress()));

    const bool validSplit = ValidSplitIndex(scope.shadowMapDataIndex);
    auto& cache = CacheForSplit(validSplit ? scope.shadowMapDataIndex : 0);
    auto& depthCache = DepthCacheForSplit(validSplit ? scope.shadowMapDataIndex : 0);
    auto& ctx = ContextForSplit(validSplit ? scope.shadowMapDataIndex : 0);
    const bool preCacheValid = cache.valid;
    const bool preDepthValid = depthCache.valid;
    const bool preStableKey = CacheKeyStable(cache.key, scope.key);
    const bool cacheEligible = IsDirectionalSplitCacheEligible(scope);
    const bool contextMatches = cacheEligible && ContextMatchesScope(ctx, scope);
    const bool cacheHit = contextMatches && ctx.cacheHit;

    tl_scopes.push_back(scope);

    const char* decision = "unknown";
    if (contextMatches && ctx.buildBoth) {
        decision = "build-split-overlay";
        if (!RunSplitBuildShadowMap(ctx, light, shadowMapData, scope)) {
            decision = "fallback-split-failed";
            if (!RunScratchFullFallbackShadowMap(ctx, light, shadowMapData) &&
                !RunRoutedPassthroughShadowMap(ctx, light, shadowMapData)) {
                RunPassthroughShadowMap(light, shadowMapData);
            }
        }
    } else if (contextMatches && ctx.cacheHit) {
        decision = "dynamic-overlay";
        if (!RunCachedDynamicShadowMap(ctx, light, shadowMapData, scope)) {
            decision = "fallback-restore-failed";
            RunPassthroughShadowMap(light, shadowMapData);
        } else if (ctx.fullFallbackThisFrame) {
            decision = "dynamic-overlay-full-fallback";
        }
    } else if (cacheEligible) {
        decision = ctx.splitEmpty ? "passthrough-routed-empty" :
                   ctx.splitFailed ? "passthrough-split-setup-failed" :
                   "passthrough-unstable";
        RunPassthroughShadowMap(light, shadowMapData);
    } else {
        decision = "passthrough";
        RunOriginalRenderShadowMap(light, shadowMapData);
    }

    Scope finished = tl_scopes.back();

    if (telemetryOn) {
        const auto t1 = std::chrono::steady_clock::now();
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - finished.start).count());

        AddToAggregate(s_total, finished, ns);
        AddToAggregate(s_byKey[finished.key], finished, ns);
    }
    if (!cacheEligible) {
        MarkDirectionalSplitCacheUpdated(finished);
    }
    MaybeLogShadowCache(
        finished,
        cache,
        depthCache,
        decision,
        cacheEligible,
        cacheHit,
        preCacheValid,
        preDepthValid,
        preStableKey);
    if (telemetryOn) {
        MaybeLog();
    }

    tl_scopes.pop_back();
    tl_shadowCachePass = ShadowCachePass::None;
    tl_shadowCacheRenderSplit = kInvalidSplitIndex;
}

void HookedDestroyRenderTargets(void* renderTargetManager)
{
    if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        REX::INFO("ShadowCache: RenderTargetManager::DestroyRenderTargets invalidating static depth caches");
        ResetShadowCacheState();
    }

    if (s_origDestroyRenderTargets) {
        s_origDestroyRenderTargets(renderTargetManager);
    }
}

void RestoreStaticCacheAfterVanillaFlush()
{
    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
        !ValidSplitIndex(tl_shadowCacheRenderSplit)) {
        return;
    }

    const ShadowCachePass pass = tl_shadowCachePass;
    if (pass != ShadowCachePass::StaticRender &&
        pass != ShadowCachePass::DynamicRender) {
        return;
    }

    auto& ctx = ContextForSplit(tl_shadowCacheRenderSplit);
    auto* dsv = ActiveShadowDSVFromRendererState(ctx.key);
    if (dsv) {
        ctx.activeDepthStencilView = dsv;
    }

    if (pass == ShadowCachePass::StaticRender) {
        return;
    }

    auto& depthCache = DepthCacheForSplit(ctx.slotIndex);
    if (ctx.dynamicRestoreSucceeded ||
        (ctx.staticRenderedThisFrame && !ctx.staticCaptureSucceeded && !depthCache.valid)) {
        return;
    }

    ctx.dynamicRestoreAttempted = true;
    auto& cache = CacheForSplit(ctx.slotIndex);
    const bool restored = RestoreStaticCacheToDSV(
        depthCache,
        ActiveD3DContext(),
        dsv,
        ctx.slotIndex,
        ctx.key);
    if (!restored) {
        ++cache.restoreFailures;
        ctx.restoreFailedThisFrame = true;
        cache.valid = false;
        ReleaseStaticDepthCache(depthCache);
        return;
    }

    ctx.dynamicRestoreSucceeded = true;
}

void HookedRenderShadowMapFlush(void* renderer)
{
    if (s_origRenderShadowMapFlush) {
        s_origRenderShadowMapFlush(renderer);
    }
    RestoreStaticCacheAfterVanillaFlush();
}

bool PatchCall(const char* label, std::uintptr_t callSite, void* hook, std::uintptr_t* outOriginal)
{
    REL::Relocation<std::uintptr_t> rel{ callSite };
    const auto original = rel.write_call<5>(hook);
    if (outOriginal && !*outOriginal) {
        *outOriginal = original;
    }
    REX::INFO("ShadowTelemetry::Initialize: patched {} call site @ {:#x} -> original {:#x}",
              label, callSite, original);
    return true;
}

}  // anonymous namespace

void OnBSDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty()) return;
    ++tl_scopes.back().bsDraws;
    if (!tl_workScopes.empty()) {
        ++tl_workScopes.back().bsDraws;
    }
}

void OnD3DDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty()) return;
    ++tl_scopes.back().d3dDraws;
    if (!tl_workScopes.empty()) {
        ++tl_workScopes.back().d3dDraws;
    }
}

void OnCommandBufferDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty()) return;
    ++tl_scopes.back().cmdBufDraws;
    if (!tl_workScopes.empty()) {
        ++tl_workScopes.back().cmdBufDraws;
    }
}

bool BeginShadowWork(WorkKind kind, const WorkTarget& target)
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty()) {
        return false;
    }

    const auto& shadow = tl_scopes.back().key;
    WorkScope scope;
    scope.key.shadow = shadow;
    scope.key.kind = kind;
    scope.key.owner = target.owner;
    scope.key.head = target.head;
    scope.key.cbData = target.cbData;
    scope.key.commandBuffer = target.commandBuffer;
    scope.key.geometry = target.geometry;
    scope.key.shader = target.shader;
    scope.key.shaderProperty = target.shaderProperty;
    scope.key.techniqueID = target.techniqueID;
    scope.key.passGroupIdx = target.passGroupIdx;
    scope.key.subIdx = target.subIdx;
    scope.key.chainLen = target.chainLen;
    scope.key.srvCount = target.srvCount;
    scope.key.cbRecordCount = target.cbRecordCount;
    scope.key.cbDrawCount = target.cbDrawCount;
    scope.key.cbNonZeroDrawCount = target.cbNonZeroDrawCount;
    scope.key.cbMaxDrawCount = target.cbMaxDrawCount;
    scope.key.cbSrvRecordCount = target.cbSrvRecordCount;
    scope.key.cbMaxSrvRecordCount = target.cbMaxSrvRecordCount;
    scope.key.allowAlpha = target.allowAlpha;
    scope.start = std::chrono::steady_clock::now();
    tl_workScopes.push_back(scope);
    return true;
}

void EndShadowWork(WorkKind kind)
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || tl_scopes.empty() || tl_workScopes.empty()) {
        return;
    }

    WorkScope scope = tl_workScopes.back();
    tl_workScopes.pop_back();
    if (scope.key.kind != kind) {
        return;
    }

    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - scope.start).count());

    const auto idx = WorkIndex(scope.key.kind);
    AddToWorkAggregate(s_workTotals[idx], ns, scope.bsDraws, scope.d3dDraws, scope.cmdBufDraws);
    AddToWorkAggregate(s_workByKey[scope.key], ns, scope.bsDraws, scope.d3dDraws, scope.cmdBufDraws);
    AddToWorkAggregate(tl_scopes.back().work[idx], ns, scope.bsDraws, scope.d3dDraws, scope.cmdBufDraws);
}

bool IsInShadowMap()
{
    return !tl_scopes.empty();
}

bool IsDirectionalSplitShadow()
{
    return g_shadowCacheTargetActive.load(std::memory_order_acquire) &&
           ValidSplitIndex(g_shadowCacheTargetSplit.load(std::memory_order_acquire));
}

bool IsShadowCacheActiveForCurrentShadow()
{
    return SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON &&
           IsDirectionalSplitShadow();
}

bool IsShadowCacheStaticBuildPass()
{
    return IsShadowCacheActiveForCurrentShadow() &&
           CurrentResolvedRegistrationPass() == ShadowCachePass::StaticRender;
}

bool IsShadowCacheDynamicOverlayPass()
{
    return IsShadowCacheActiveForCurrentShadow() &&
           CurrentResolvedRegistrationPass() == ShadowCachePass::DynamicRender;
}

bool IsShadowCacheRegistrationFilterActive(void* accumulator, void* geometry)
{
    (void)geometry;
    const auto splitIndex = FindRegistrationSplitForAccumulator(accumulator);
    if (!ValidSplitIndex(splitIndex)) {
        return false;
    }
    return g_registrationBuildBothBySplit[splitIndex].load(std::memory_order_acquire) ||
           g_registrationDynamicOnlyBySplit[splitIndex].load(std::memory_order_acquire);
}

bool RouteShadowCacheRegistration(void* accumulator, bool isStaticCaster, void** outBatchRenderer)
{
    if (outBatchRenderer) {
        *outBatchRenderer = nullptr;
    }
    if (!outBatchRenderer ||
        !g_registrationShadowCacheActive.load(std::memory_order_acquire)) {
        return false;
    }

    const auto splitIndex = FindRegistrationSplitForAccumulator(accumulator);
    tl_lastRegistrationSplit = splitIndex;
    if (!ValidSplitIndex(splitIndex)) {
        return false;
    }

    const bool buildBoth = g_registrationBuildBothBySplit[splitIndex].load(std::memory_order_acquire);
    const bool dynamicOnly = g_registrationDynamicOnlyBySplit[splitIndex].load(std::memory_order_acquire);
    if (!buildBoth && !dynamicOnly) {
        return false;
    }
    if (dynamicOnly && isStaticCaster) {
        return true;
    }

    void* targetAccumulator = isStaticCaster ?
        g_registrationStaticAccumulators[splitIndex].load(std::memory_order_acquire) :
        g_registrationDynamicAccumulators[splitIndex].load(std::memory_order_acquire);
    if (!targetAccumulator) {
        return false;
    }

    *outBatchRenderer = static_cast<std::byte*>(targetAccumulator) + kAccumulatorBatchRendererOffset;
    return true;
}

void NoteShadowCacheShadowMapOrMaskHook(bool active)
{
    if constexpr (!kShadowCacheLogEnabled) {
        return;
    }

    const ShadowCachePass pass = CurrentResolvedRegistrationPass();
    const std::size_t passIdx = PassIndex(pass);
    if (!ValidSplitIndex(tl_lastRegistrationSplit)) {
        return;
    }
    auto& cache = CacheForSplit(tl_lastRegistrationSplit);
    IncrementRegistrationCounter(cache.shadowMapOrMaskHookCalls);
    IncrementRegistrationCounter(cache.shadowMapOrMaskHookCallsByPass[passIdx]);
    if (active) {
        IncrementRegistrationCounter(cache.shadowMapOrMaskHookActiveCalls);
        IncrementRegistrationCounter(cache.shadowMapOrMaskHookActiveCallsByPass[passIdx]);
    }
}

void NoteShadowCacheShadowMapOrMaskHookDetail(bool active, void* accumulator)
{
    if constexpr (!kShadowCacheLogEnabled) {
        return;
    }

    tl_lastRegistrationSplit = FindRegistrationSplitForAccumulator(accumulator);
    NoteShadowCacheShadowMapOrMaskHook(active);
}

void NoteShadowCacheRenderPassSplit(bool kept, bool isStaticCaster)
{
    if constexpr (!kShadowCacheLogEnabled) {
        return;
    }

    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
        !g_shadowCacheTargetActive.load(std::memory_order_acquire) ||
        !ValidSplitIndex(tl_shadowCacheRenderSplit)) {
        return;
    }

    auto& cache = CacheForSplit(tl_shadowCacheRenderSplit);
    switch (CurrentResolvedRegistrationPass()) {
    case ShadowCachePass::BuildBoth:
        if (kept && isStaticCaster) {
            IncrementRegistrationCounter(cache.staticKept);
            g_registrationStaticKeepCounts[tl_shadowCacheRenderSplit].fetch_add(1, std::memory_order_relaxed);
        } else {
            IncrementRegistrationCounter(cache.staticSkipped);
        }
        break;
    case ShadowCachePass::DynamicRender:
        if (kept) {
            IncrementRegistrationCounter(cache.overlayKept);
        } else {
            IncrementRegistrationCounter(cache.overlaySkipped);
        }
        break;
    default:
        break;
    }
}

void NoteShadowCachePassRouted(bool isStaticCaster)
{
    if (!ValidSplitIndex(tl_lastRegistrationSplit)) {
        return;
    }
    if (isStaticCaster) {
        if constexpr (kShadowCacheLogEnabled) {
            IncrementRegistrationCounter(CacheForSplit(tl_lastRegistrationSplit).staticKept);
        }
        g_registrationStaticKeepCounts[tl_lastRegistrationSplit].fetch_add(1, std::memory_order_relaxed);
    } else {
        if constexpr (kShadowCacheLogEnabled) {
            IncrementRegistrationCounter(CacheForSplit(tl_lastRegistrationSplit).overlayKept);
        }
        g_registrationDynamicKeepCounts[tl_lastRegistrationSplit].fetch_add(1, std::memory_order_relaxed);
    }
}

void ResetShadowCacheState()
{
    ReleaseStaticDepthCaches();
    ClearAllScratchAccumulators();
    ResetDirectionalSplitFrameContexts();
    for (auto& cache : s_directionalSplitCaches) {
        cache.valid = false;
        cache.skipsSinceUpdate = 0;
        cache.stableKeyHits = 0;
        cache.pendingBuildKey = {};
        cache.pendingBuildKeyHits = 0;
        cache.key = {};
    }
    ClearRegistrationTarget();
    ResetWindow();
}

bool HandleShadowCacheClearDepthStencilView(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11DepthStencilView* dsv,
    std::uint32_t clearFlags,
    float depth,
    std::uint8_t stencil)
{
    (void)context;
    if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        return false;
    }
    const bool targetActive = g_shadowCacheTargetActive.load(std::memory_order_acquire) &&
        ValidSplitIndex(tl_shadowCacheRenderSplit);
    if (!targetActive) {
        MaybeLogShadowCacheClearReject(
            "target-inactive",
            g_shadowCacheClearRejectTargetInactive,
            dsv,
            clearFlags,
            depth,
            stencil);
        return false;
    }
    const std::uint32_t activeSplit = tl_shadowCacheRenderSplit;
    const ShadowCachePass activePass = tl_shadowCachePass;
    if (!ValidSplitIndex(activeSplit)) {
        MaybeLogShadowCacheClearReject(
            "split-invalid",
            g_shadowCacheClearRejectSplitInvalid,
            dsv,
            clearFlags,
            depth,
            stencil);
        return false;
    }

    auto& ctx = ContextForSplit(activeSplit);
    auto& depthCache = DepthCacheForSplit(activeSplit);
    if (!ShadowPassDSVMatches(ctx, dsv)) {
        MaybeLogShadowCacheClearReject(
            "dsv-mismatch",
            g_shadowCacheClearRejectDSVMismatch,
            dsv,
            clearFlags,
            depth,
            stencil);
        return false;
    }
    ctx.activeDepthStencilView = dsv;

    if (activePass == ShadowCachePass::StaticRender) {
        return false;
    }

    if (activePass != ShadowCachePass::DynamicRender) {
        return false;
    }

    if (ctx.staticRenderedThisFrame && !ctx.staticCaptureSucceeded && !depthCache.valid) {
        return true;
    }

    return false;
}

bool Initialize()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On &&
        !SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        REX::INFO("ShadowTelemetry::Initialize: mode=off and shadow cache=off; hooks skipped");
        return true;
    }

    if (REX::FModule::GetRuntimeIndex() != REX::FModule::Runtime::kOG) {
        REX::WARN("ShadowTelemetry::Initialize: hooks skipped; call-site offsets are verified for OG runtime only");
        return false;
    }

    const bool needsRenderSceneHook = g_mode.load(std::memory_order_relaxed) == Mode::On;
    const bool alreadyReady =
        s_origRenderShadowMap &&
        (!needsRenderSceneHook || s_origRenderScene) &&
        (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
            (s_origAccumulateFromLists && s_origRenderShadowMapFlush && s_origDestroyRenderTargets));
    if (s_installed && alreadyReady) {
        REX::INFO("ShadowTelemetry::Initialize: already installed; skipping");
        return true;
    }

    bool ok = true;
    std::uintptr_t originalShadow = 0;
    std::uintptr_t originalFlush = 0;
    std::uintptr_t originalScene = 0;

    if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON && !s_origDestroyRenderTargets) {

        s_origDestroyRenderTargets = Hooks::CreateBranchGateway5<RenderTargetManagerDestroyRenderTargets_t>(
            ptr_DestroyRenderTargets,
            kDestroyRenderTargetsPrologueSizeOG,
            reinterpret_cast<void*>(&HookedDestroyRenderTargets));
        if (!s_origDestroyRenderTargets) {
            REX::WARN("ShadowTelemetry::Initialize: failed to install RenderTargetManager::DestroyRenderTargets hook");
            return false;
        }
        REX::INFO("ShadowTelemetry::Initialize: RenderTargetManager::DestroyRenderTargets hook installed");
    }

    if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON && !s_origAccumulateFromLists) {

        s_origAccumulateFromLists = Hooks::CreateBranchGateway5<AccumulateFromLists_t>(
            ptr_AccumulateFromLists,
            kAccumulateFromListsPrologueSizeOG,
            reinterpret_cast<void*>(&HookedAccumulateFromLists));
        if (!s_origAccumulateFromLists) {
            REX::WARN("ShadowTelemetry::Initialize: failed to install BSShadowDirectionalLight::AccumulateFromLists hook");
            return false;
        }
        REX::INFO("ShadowTelemetry::Initialize: BSShadowDirectionalLight::AccumulateFromLists hook installed");
    }

    if (needsRenderSceneHook) {
        const auto renderShadowMapAddr = ptr_RenderShadowMap.address();
        if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON && !s_origRenderShadowMapFlush) {
            ok &= PatchCall("BSShadowLight::RenderShadowMap -> Renderer::Flush",
                            renderShadowMapAddr + kRenderShadowMapFlushCallOffsetOG,
                            reinterpret_cast<void*>(&HookedRenderShadowMapFlush),
                            &originalFlush);
            s_origRenderShadowMapFlush = reinterpret_cast<RendererFlush_t>(originalFlush);
        }
        if (!s_origRenderScene) {
            ok &= PatchCall("BSShadowLight::RenderShadowMap -> BSShaderUtil::RenderScene",
                            renderShadowMapAddr + kRenderSceneCallOffsetOG,
                            reinterpret_cast<void*>(&HookedRenderScene),
                            &originalScene);
            s_origRenderScene = reinterpret_cast<RenderScene_t>(originalScene);
        }
    } else if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON && !s_origRenderShadowMapFlush) {
        const auto renderShadowMapAddr = ptr_RenderShadowMap.address();
        ok &= PatchCall("BSShadowLight::RenderShadowMap -> Renderer::Flush",
                        renderShadowMapAddr + kRenderShadowMapFlushCallOffsetOG,
                        reinterpret_cast<void*>(&HookedRenderShadowMapFlush),
                        &originalFlush);
        s_origRenderShadowMapFlush = reinterpret_cast<RendererFlush_t>(originalFlush);
    }

    if (!s_origRenderShadowMap) {
        ok &= PatchCall("BSShadowDirectionalLight::Render -> RenderShadowMap",
                        ptr_ShadowDirectionalRender.address() + kDirectionalRenderShadowMapCallOffsetOG,
                        reinterpret_cast<void*>(&HookedRenderShadowMap),
                        &originalShadow);
        s_origRenderShadowMap = reinterpret_cast<RenderShadowMap_t>(originalShadow);
    }

    if (!s_origRenderShadowMap ||
        (needsRenderSceneHook && !s_origRenderScene) ||
        (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON && (!s_origAccumulateFromLists || !s_origRenderShadowMapFlush || !s_origDestroyRenderTargets))) {
        REX::WARN("ShadowTelemetry::Initialize: original function capture failed (RenderShadowMap={}, RenderScene={}, Flush={}, AccumulateFromLists={}, DestroyRenderTargets={}, telemetryMode={})",
                  reinterpret_cast<void*>(s_origRenderShadowMap),
                  reinterpret_cast<void*>(s_origRenderScene),
                  reinterpret_cast<void*>(s_origRenderShadowMapFlush),
                  reinterpret_cast<void*>(s_origAccumulateFromLists),
                  reinterpret_cast<void*>(s_origDestroyRenderTargets),
                  static_cast<int>(g_mode.load(std::memory_order_relaxed)));
        return false;
    }

    s_lastLogTime = std::chrono::steady_clock::now();
    s_installed = ok;
    REX::INFO("ShadowTelemetry::Initialize: hooks {} logging every {:.1f}s",
              ok ? "installed" : "partially installed", kLogIntervalSecs);
    return ok;
}

}  // namespace ShadowTelemetry

#endif
