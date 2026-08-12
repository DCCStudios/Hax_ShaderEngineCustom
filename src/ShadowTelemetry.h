#pragma once

#include <FeatureGates.h>

#include <atomic>
#include <cstdint>

namespace REX::W32 {
    struct ID3D11DeviceContext;
    struct ID3D11DepthStencilView;
}

// ShadowTelemetry attributes shadow-map submission cost hidden inside
// DrawWorld::DeferredLightsImpl and owns the optional directional shadow
// static-cache experiment.
namespace ShadowTelemetry {

enum class Mode : std::uint8_t {
    Off,
    On,
};

extern std::atomic<Mode> g_mode;
inline constexpr bool kDetailedShadowCacheLogging = false;

// One directional shadow cascade as the engine set it up this frame.
//
// `transform` is BSShadowLight::ShadowmapDescriptor::lightTransform, read at
// descriptor offset 0x00. That offset is derived from Skyrim's documented
// ShadowmapDescriptor layout, corroborated by five neighbouring offsets this
// file already reads successfully (camera 0x40, accumulator 0x48, port 0xD0,
// cullingProcess 0xE0, and the 0xF0 stride). It is NOT independently observed
// on Fallout yet, so consumers must treat it as unvalidated: check
// `SunCascadesLookValid()` and compare against a LightTracker cb2 dump
// (cb2[11..13] is the engine's own cascade-0 projection) before trusting it.
//
// `mapSlot` is the shadow texture array slice this cascade renders into.
struct DirectionalCascade {
    float         transform[16] = {};
    std::uint32_t mapSlot       = 0;
    bool          valid         = false;
};

inline constexpr std::uint32_t kMaxPublishedCascades = 4;

// Copies up to `maxCount` cascades captured during the last directional
// shadow submission. Returns how many were written.
std::uint32_t GetDirectionalCascades(
    DirectionalCascade* out, std::uint32_t maxCount) noexcept;

// True when every live cascade transform is finite and non-degenerate. This
// is the cheap runtime gate that keeps a wrong offset from producing garbage
// shadows rather than an obvious failure.
bool SunCascadesLookValid() noexcept;

#if SHADERENGINE_ENABLE_SHADOW_TELEMETRY || SHADERENGINE_ENABLE_SHADOW_CACHE

bool Initialize();

enum class WorkKind : std::uint8_t {
    CommandBufferReplay,
    ImmediatePass,
    BuildCommandBuffer,
};

struct WorkTarget {
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
};

// Called from the existing renderer hooks. Cheap when mode is off, and only
// increments counters while BSShadowLight::RenderShadowMap is on the stack.
void OnBSDraw();
void OnD3DDraw();
void OnCommandBufferDraw();

bool BeginShadowWork(WorkKind kind, const WorkTarget& target);
void EndShadowWork(WorkKind kind);

bool IsInShadowMap();
bool IsDirectionalSplitShadow();
bool IsShadowCacheActiveForCurrentShadow();
bool IsShadowCacheStaticBuildPass();
bool IsShadowCacheDynamicOverlayPass();
bool IsShadowCacheRegistrationFilterActive(void* accumulator, void* geometry);
// Returns true when the current shadow-cache registration route handled the
// pass. *outBatchRenderer == nullptr means the pass should be skipped.
bool RouteShadowCacheRegistration(void* accumulator, bool isStaticCaster, void** outBatchRenderer);
void NoteShadowCacheShadowMapOrMaskHook(bool active);
void NoteShadowCacheShadowMapOrMaskHookDetail(bool active, void* accumulator);
void NoteShadowCacheRenderPassSplit(bool kept, bool isStaticCaster);
void NoteShadowCachePassRouted(bool isStaticCaster);
void ResetShadowCacheState();

// Called from the D3D11 ClearDepthStencilView hook. Returns true when the
// hook handled the clear by restoring cached static depth and the original
// ClearDepthStencilView call must be suppressed.
bool HandleShadowCacheClearDepthStencilView(
    REX::W32::ID3D11DeviceContext* context,
    REX::W32::ID3D11DepthStencilView* dsv,
    std::uint32_t clearFlags,
    float depth,
    std::uint8_t stencil);

#else

inline std::atomic<Mode> g_mode{ Mode::Off };

enum class WorkKind : std::uint8_t {
    CommandBufferReplay,
    ImmediatePass,
    BuildCommandBuffer,
};

struct WorkTarget {
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
};

inline bool Initialize() { return true; }
inline std::uint32_t GetDirectionalCascades(DirectionalCascade*, std::uint32_t) noexcept { return 0; }
inline bool SunCascadesLookValid() noexcept { return false; }
inline void OnBSDraw() {}
inline void OnD3DDraw() {}
inline void OnCommandBufferDraw() {}
inline bool BeginShadowWork(WorkKind, const WorkTarget&) { return false; }
inline void EndShadowWork(WorkKind) {}
inline bool IsInShadowMap() { return false; }
inline bool IsDirectionalSplitShadow() { return false; }
inline bool IsShadowCacheActiveForCurrentShadow() { return false; }
inline bool IsShadowCacheStaticBuildPass() { return false; }
inline bool IsShadowCacheDynamicOverlayPass() { return false; }
inline bool IsShadowCacheRegistrationFilterActive(void*, void*) { return false; }
inline bool RouteShadowCacheRegistration(void*, bool, void**) { return false; }
inline void NoteShadowCacheShadowMapOrMaskHook(bool) {}
inline void NoteShadowCacheShadowMapOrMaskHookDetail(bool, void*) {}
inline void NoteShadowCacheRenderPassSplit(bool, bool) {}
inline void NoteShadowCachePassRouted(bool) {}
inline void ResetShadowCacheState() {}
inline bool HandleShadowCacheClearDepthStencilView(
    REX::W32::ID3D11DeviceContext*,
    REX::W32::ID3D11DepthStencilView*,
    std::uint32_t,
    float,
    std::uint8_t)
{
    return false;
}

#endif

}  // namespace ShadowTelemetry
