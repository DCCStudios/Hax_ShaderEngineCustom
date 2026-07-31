#pragma once

#include <FeatureGates.h>

#include <atomic>
#include <cstdint>

// PhaseTelemetry — per-phase timing + draw-count instrumentation for the
// G-buffer pass.
//
// `FinishAccumulating_Standard_PreResolveDepth` (and its Post sibling) is a
// flat sequence of ~10 sub-calls. RenderDoc shows the parent costs ~16 ms in
// Boston but there is no per-sub-call breakdown anywhere — we don't know
// whether RenderOpaqueDecals owns 5 ms or 0.5 ms. That's the question this
// module answers.
//
// What we measure, per logging window, per `{Pre|Post}` × `{subPhase}`:
//   - call count
//   - draw count (via OnDraw, called from HookedBSBatchRendererDraw)
//   - total ns
//   - max ns (single-call worst case)
//
// Sub-phases (mapped to known hook targets):
//   - OpaqueDecals      — BSShaderAccumulator::RenderOpaqueDecals
//   - AllBatches        — BSShaderAccumulator::RenderAllBatches (its inner
//                          13-group loop fires through BSBatchRenderer; all
//                          attribute here)
//   - BlendedDecals     — BSShaderAccumulator::RenderBlendedDecals
//   - SortAlphaPasses   — BSBatchRenderer::SortAlphaPasses
//   - GG[g]             — BSShaderAccumulator::RenderGeometryGroup, binned by
//                          the `group` argument (0..19)
//   - <total>           — the outer Pre/Post wrapper's own wall time
//   - <unaccounted>     — computed at log time as total − Σ children. Picks up
//                          the direct `BSBatchRenderer::RenderBatches(group=7)`
//                          call inside PreResolveDepth and any inlined helpers
//                          we don't separately hook.
//
// Each child hook checks the TLS phase counter and only attributes if invoked
// from within Pre or Post. Sub-functions called from outside (e.g. the
// shadow path, immediate-mode menu rendering) are passed through to the
// original without telemetry.
//
// INI gate: `PHASE_TELEMETRY_MODE = off | on`. Default off — zero impact.

namespace PhaseTelemetry {

enum class Mode {
    Off,
    On,
};

extern std::atomic<Mode> g_mode;

// Force-install the DrawWorld wrappers even when telemetry logging is off.
// Rendering-side features use these wrappers as cheap phase context only.
void RequireHooks();

// Cheap render-thread phase predicates used by renderer hooks.
bool IsInRenderPreUI();
bool IsInMainAccum();
bool IsInDeferredPrePass();
bool IsInDeferredLightsImpl();

// DeferredLightsImpl hook ownership lives in ShadowUpgrade. These observer
// callbacks preserve telemetry attribution without making telemetry a
// prerequisite for renderer features.
void BeginDeferredLightsImpl();
void EndDeferredLightsImpl();

enum class DeferredPrePassDetailKind : std::uint8_t {
    RenderBatches,
    RenderGeometryGroup,
    RenderCommandBufferPasses,
    RenderPassImpl,
};

enum class CommandBufferD3DCallKind : std::uint8_t {
    Map,
    Unmap,
    ConstantBuffer,
    ShaderResource,
    ShaderResourceSkip,
    Sampler,
    InputAssembly,
    InputAssemblySkip,
    State,
    Draw,
    ResourceWait,
    ResourceFlush,
    ResourceEscalate,
    Count,
};

#if SHADERENGINE_ENABLE_PHASE_TELEMETRY

// Optional fine-grain profiling inside DrawWorld::DeferredPrePass. These are
// inclusive scopes; RenderGeometryGroup can contain nested RenderBatches time.
// RenderCommandBufferPasses uses key = (group << 5) | min(subIdx, 31).
void BeginDeferredPrePassDetail(DeferredPrePassDetailKind kind, std::uint32_t key);
void EndDeferredPrePassDetail(DeferredPrePassDetailKind kind, std::uint32_t key);
bool GetCurrentDeferredPrePassDetail(DeferredPrePassDetailKind& kind, std::uint32_t& key);
void NoteDeferredPrePassCommandBuffer(
    std::uint32_t key,
    void* head,
    void* geometry,
    void* shader,
    std::uint32_t techniqueID,
    std::uint32_t chainLen,
    const char* geometryName);

// Called from Plugin.cpp's HookedBSBatchRendererDraw. Cheap when mode==Off
// (single relaxed atomic load + branch). Attributes a draw to the currently
// active sub-phase bucket (or to the Frame total if just inside Render_PreUI
// but outside any tracked sub-phase).
void OnDraw();

// Called from the D3D11 immediate-context Draw* hooks. This is the ground-truth
// API draw-call counter; OnDraw is the higher-level BSBatchRenderer counter.
void OnD3DDraw();

// Called from BSBatchRenderer::RenderCommandBufferPassesImpl. This captures
// Bethesda's pre-recorded command-buffer draw path, which bypasses
// BSBatchRenderer::Draw and may also bypass our immediate-context hooks.
void OnCommandBufferDraw();
void EnterCommandBufferReplay();
void LeaveCommandBufferReplay();
bool IsInCommandBufferReplay();
void OnCommandBufferD3DCall(CommandBufferD3DCallKind kind, std::uint64_t ns);

// Install the DrawWorld:: hooks. Hooks are installed if PHASE_TELEMETRY_MODE is
// `on` OR if any piggy-back consumer (LightSorter) is enabled — see
// PhaseTelemetry.cpp::Initialize. Safe to call multiple times. Returns false
// if any individual hook install fails; partial-install state is logged.
bool Initialize();

#else

inline std::atomic<Mode> g_mode{ Mode::Off };
inline thread_local std::uint32_t g_commandBufferReplayDepth = 0;

inline void RequireHooks() {}
inline bool IsInRenderPreUI() { return false; }
inline bool IsInMainAccum() { return false; }
inline bool IsInDeferredPrePass() { return false; }
inline bool IsInDeferredLightsImpl() { return false; }
inline void BeginDeferredLightsImpl() {}
inline void EndDeferredLightsImpl() {}
inline void BeginDeferredPrePassDetail(DeferredPrePassDetailKind, std::uint32_t) {}
inline void EndDeferredPrePassDetail(DeferredPrePassDetailKind, std::uint32_t) {}
inline bool GetCurrentDeferredPrePassDetail(DeferredPrePassDetailKind&, std::uint32_t&) { return false; }
inline void NoteDeferredPrePassCommandBuffer(
    std::uint32_t,
    void*,
    void*,
    void*,
    std::uint32_t,
    std::uint32_t,
    const char*) {}
inline void OnDraw() {}
inline void OnD3DDraw() {}
inline void OnCommandBufferDraw() {}
inline void EnterCommandBufferReplay() { ++g_commandBufferReplayDepth; }
inline void LeaveCommandBufferReplay()
{
    if (g_commandBufferReplayDepth > 0) {
        --g_commandBufferReplayDepth;
    }
}
inline bool IsInCommandBufferReplay() { return g_commandBufferReplayDepth > 0; }
inline void OnCommandBufferD3DCall(CommandBufferD3DCallKind, std::uint64_t) {}
inline bool Initialize() { return true; }

#endif

}  // namespace PhaseTelemetry
