#include <PCH.h>
#include "PhaseTelemetry.h"

#if SHADERENGINE_ENABLE_PHASE_TELEMETRY

#include "hooks.h"

#include <chrono>
#include <cstring>

namespace PhaseTelemetry {

std::atomic<Mode> g_mode{ Mode::Off };

namespace {

std::atomic<bool> s_forceHooks{ false };

// --- Hook targets ----------------------------------------------------------
//
// The per-frame world render entry on OG 1.10.163 is
//   Main::DrawWorld_And_UI                @ 0x140D3CBE0
//     -> DrawWorld::Render_PreUI          @ 0x142857480
//          -> DrawWorld::LightUpdate
//          -> DrawWorld::MainAccum            (cull + AccumulateSceneArray)
//          -> DrawWorld::MainRenderSetup
//          -> DrawWorld::OpaqueWireframe
//          -> DrawWorld::DeferredPrePass      real G-buffer Pre
//          -> DrawWorld::UpdateMotionBlur
//          -> DrawWorld::DeferredDecals
//          -> DrawWorld::ImagespaceSAO
//          -> DrawWorld::NvidiaHBAO           (conditional)
//          -> DrawWorld::DeferredLightsImpl
//          -> DrawWorld::DeferredComposite
//          -> DrawWorld::Forward              (alpha; eventually calls Post)
//          -> DrawWorld::Refraction
//
// We hook Render_PreUI as the outer "Frame" bucket and each *hookable*
// DrawWorld:: sub-phase as its own bucket. Five sub-phases are intentionally
// skipped because their prologues contain a RIP-relative memory access inside
// the first 14 bytes with no preceding clean boundary at byte ??5
// (LightUpdate / OpaqueWireframe / UpdateMotionBlur / ImagespaceSAO /
// Refraction). Our gateway templates don't relocate rel32 displacements; their
// wall time is tracked directly by the relocated gateway path.
//
// OG REL::IDs are from tools/version-1-10-163-0-e.txt. AE IDs are set to 0
// per user direction ("I don't care about NG/AE crashing") ??the Initialize()
// path declines to install if the resolved address is outside .text. On AE
// the entire module no-ops cleanly; no telemetry, but no crash either.
//
// Per-target prologue sizes (hand-disassembled in OG):
//   Render_PreUI        984743   E9-5   8B   mov r11,rsp; push rsi; sub rsp,0x70
//                                            (RIP-rel movzx at byte 8)
//   MainAccum           718911   E9-5   9B   push rbx; push rsi; push r13; sub rsp,0x50
//                                            (RIP-rel mov at byte 9)
//   MainRenderSetup     339369   E9-5   6B   sub rsp,0x38; xor edx,edx
//                                            (RIP-rel lea at byte 6)
//   DeferredPrePass      56596   E9-5  15B   stack save mov [rsp+disp8],reg
//   DeferredDecals      631771   E9-5  17B   mov [rsp+0x18],rbx + 5 pushes + lea rbp
//   NvidiaHBAO          253972   E9-5  15B   mov rax,rsp + 3 saves
//   DeferredLightsImpl is owned by ShadowUpgrade and reported through
//   BeginDeferredLightsImpl/EndDeferredLightsImpl.
//   DeferredComposite   728427   E9-5  15B   mov rax,rsp + 2 saves + 5 pushes
//   Forward             656535   CALL-5 from Render_PreUI+0x1C9
//                                            (avoids detouring the Forward prologue,
//                                             which frame-generation/upscaling mods
//                                             also hook)

REL::Relocation<std::uintptr_t> ptr_RenderPreUI        { REL::ID{  984743, 0 } };
REL::Relocation<std::uintptr_t> ptr_MainAccum          { REL::ID{  718911, 0 } };
REL::Relocation<std::uintptr_t> ptr_MainRenderSetup    { REL::ID{  339369, 0 } };
REL::Relocation<std::uintptr_t> ptr_DeferredPrePass    { REL::ID{   56596, 0 } };
REL::Relocation<std::uintptr_t> ptr_DeferredDecals     { REL::ID{  631771, 0 } };
REL::Relocation<std::uintptr_t> ptr_NvidiaHBAO         { REL::ID{  253972, 0 } };
REL::Relocation<std::uintptr_t> ptr_DeferredComposite  { REL::ID{  728427, 0 } };
REL::Relocation<std::uintptr_t> ptr_Forward            { REL::ID{  656535, 0 } };
// Tier 0: the five "skipped" phases, hooked via relocating gateway.
// All five share the prologue pattern `sub rsp,0x38` (4B) + one 7B RIP-rel
// instruction (mov [rip+disp], cmp [rip+disp], 0). prologueSize=11 captures
// both; the gateway relocator adjusts the single RIP-rel disp32.
REL::Relocation<std::uintptr_t> ptr_LightUpdate        { REL::ID{  102390, 0 } };
REL::Relocation<std::uintptr_t> ptr_OpaqueWireframe    { REL::ID{ 1268987, 0 } };
REL::Relocation<std::uintptr_t> ptr_UpdateMotionBlur   { REL::ID{ 1051891, 0 } };
REL::Relocation<std::uintptr_t> ptr_ImagespaceSAO      { REL::ID{   39691, 0 } };
REL::Relocation<std::uintptr_t> ptr_Refraction         { REL::ID{ 1572250, 0 } };

constexpr std::size_t kPrologueRenderPreUI         =  8;  // E9-5 patch
constexpr std::size_t kPrologueMainAccum           =  9;  // E9-5 patch
constexpr std::size_t kPrologueMainRenderSetup     =  6;  // E9-5 patch
constexpr std::size_t kPrologueDeferredPrePass     = 15;  // E9-5 patch
constexpr std::size_t kPrologueDeferredDecals      = 17;  // E9-5 patch
constexpr std::size_t kPrologueNvidiaHBAO          = 15;  // E9-5 patch
constexpr std::size_t kPrologueDeferredComposite   = 15;  // E9-5 patch
constexpr std::size_t kPrologueTier0_SubAndRipRel  = 11;  // E9-5 + relocator
constexpr std::uintptr_t kRenderPreUIForwardCallOffsetOG = 0x1C9; // call DrawWorld::Forward @ 0x142857649

// All hooked functions are `void(void)` on OG.
using VoidVoid_t = void (*)();

VoidVoid_t s_origRenderPreUI        = nullptr;
VoidVoid_t s_origMainAccum          = nullptr;
VoidVoid_t s_origMainRenderSetup    = nullptr;
VoidVoid_t s_origDeferredPrePass    = nullptr;
VoidVoid_t s_origDeferredDecals     = nullptr;
VoidVoid_t s_origNvidiaHBAO         = nullptr;
VoidVoid_t s_origDeferredComposite  = nullptr;
VoidVoid_t s_origForward            = nullptr;
VoidVoid_t s_origLightUpdate        = nullptr;
VoidVoid_t s_origOpaqueWireframe    = nullptr;
VoidVoid_t s_origUpdateMotionBlur   = nullptr;
VoidVoid_t s_origImagespaceSAO      = nullptr;
VoidVoid_t s_origRefraction         = nullptr;

// Gateway creation lives in hooks.h. PhaseTelemetry uses the 5-byte gateway path everywhere,
// with the relocated variant for tier-0 prologues that contain RIP-relative instructions.

// --- Per-phase buckets ----------------------------------------------------
//
// SubPhase indexes into s_subBuckets. The "Frame" bucket holds Render_PreUI's
// own wall time + total draws issued during the frame.

enum class SubPhase : std::uint8_t {
    None = 0,
    LightUpdate,
    MainAccum,
    MainRenderSetup,
    OpaqueWireframe,
    DeferredPrePass,
    UpdateMotionBlur,
    DeferredDecals,
    ImagespaceSAO,
    NvidiaHBAO,
    DeferredLightsImpl,
    DeferredComposite,
    Forward,
    Refraction,
    Count,
};

const char* SubPhaseName(SubPhase p) noexcept
{
    switch (p) {
    case SubPhase::LightUpdate:        return "LightUpdate";
    case SubPhase::MainAccum:          return "MainAccum";
    case SubPhase::MainRenderSetup:    return "MainRenderSetup";
    case SubPhase::OpaqueWireframe:    return "OpaqueWireframe";
    case SubPhase::DeferredPrePass:    return "DeferredPrePass";
    case SubPhase::UpdateMotionBlur:   return "UpdateMotionBlur";
    case SubPhase::DeferredDecals:     return "DeferredDecals";
    case SubPhase::ImagespaceSAO:      return "ImagespaceSAO";
    case SubPhase::NvidiaHBAO:         return "NvidiaHBAO";
    case SubPhase::DeferredLightsImpl: return "DeferredLightsImpl";
    case SubPhase::DeferredComposite:  return "DeferredComposite";
    case SubPhase::Forward:            return "Forward";
    case SubPhase::Refraction:         return "Refraction";
    default:                           return "?";
    }
}

struct Bucket {
    std::atomic<std::uint64_t> calls{ 0 };
    std::atomic<std::uint64_t> draws{ 0 };
    std::atomic<std::uint64_t> d3dDraws{ 0 };
    std::atomic<std::uint64_t> cmdBufDraws{ 0 };
    std::atomic<std::uint64_t> totalNs{ 0 };
    std::atomic<std::uint64_t> maxNs{ 0 };

    void Reset() noexcept
    {
        calls.store(0, std::memory_order_relaxed);
        draws.store(0, std::memory_order_relaxed);
        d3dDraws.store(0, std::memory_order_relaxed);
        cmdBufDraws.store(0, std::memory_order_relaxed);
        totalNs.store(0, std::memory_order_relaxed);
        maxNs.store(0, std::memory_order_relaxed);
    }
};

Bucket s_frame;
Bucket s_subBuckets[static_cast<std::size_t>(SubPhase::Count)];

constexpr std::size_t kDeferredDetailGroups = 1024;
constexpr std::size_t kDeferredCommandBufferSubIdxBits = 5;
constexpr std::size_t kDeferredCommandBufferSubIdxMask = (1u << kDeferredCommandBufferSubIdxBits) - 1u;
constexpr std::size_t kDeferredDetailKinds = 4;
constexpr std::size_t kCommandBufferD3DCallKinds = static_cast<std::size_t>(CommandBufferD3DCallKind::Count);

const char* DeferredDetailKindName(DeferredPrePassDetailKind kind) noexcept
{
    switch (kind) {
    case DeferredPrePassDetailKind::RenderBatches:       return "RenderBatches";
    case DeferredPrePassDetailKind::RenderGeometryGroup: return "RenderGeometryGroup";
    case DeferredPrePassDetailKind::RenderCommandBufferPasses: return "RenderCommandBufferPasses";
    case DeferredPrePassDetailKind::RenderPassImpl:      return "RenderPassImpl";
    default:                                             return "?";
    }
}

std::string DeferredDetailKeyName(DeferredPrePassDetailKind kind, std::size_t key)
{
    if (kind != DeferredPrePassDetailKind::RenderCommandBufferPasses) {
        return std::to_string(key);
    }

    const auto group = key >> kDeferredCommandBufferSubIdxBits;
    const auto subIdx = key & kDeferredCommandBufferSubIdxMask;
    return std::to_string(group) + "." + std::to_string(subIdx);
}

struct DeferredDetailBucket {
    Bucket bucket;
    std::atomic<std::uint64_t> active{ 0 };
    void* head = nullptr;
    void* geometry = nullptr;
    void* shader = nullptr;
    std::uint32_t techniqueID = 0;
    std::uint32_t chainLen = 0;
    std::string geometryName;
    bool metadataSet = false;

    void Reset() noexcept
    {
        bucket.Reset();
        active.store(0, std::memory_order_relaxed);
        head = nullptr;
        geometry = nullptr;
        shader = nullptr;
        techniqueID = 0;
        chainLen = 0;
        geometryName.clear();
        metadataSet = false;
    }
};

DeferredDetailBucket s_deferredDetails[kDeferredDetailKinds][kDeferredDetailGroups];
Bucket s_commandBufferD3DCalls[kCommandBufferD3DCallKinds];

// thread_local ??render thread runs Render_PreUI and all its children
// sequentially. If a worker path ever entered, the bool defaults to false
// so child hooks fall through to passthrough.
thread_local bool     tl_inFrame   = false;
thread_local SubPhase tl_subphase  = SubPhase::None;
thread_local int      tl_deferredDetailIndex = -1;
thread_local std::uint32_t tl_commandBufferReplayDepth = 0;
std::atomic<bool> s_mainAccumActive{ false };

const char* CommandBufferD3DCallKindName(CommandBufferD3DCallKind kind) noexcept
{
    switch (kind) {
    case CommandBufferD3DCallKind::Map:            return "Map";
    case CommandBufferD3DCallKind::Unmap:          return "Unmap";
    case CommandBufferD3DCallKind::ConstantBuffer: return "ConstantBuffer";
    case CommandBufferD3DCallKind::ShaderResource: return "ShaderResource";
    case CommandBufferD3DCallKind::ShaderResourceSkip: return "ShaderResourceSkip";
    case CommandBufferD3DCallKind::Sampler:        return "Sampler";
    case CommandBufferD3DCallKind::InputAssembly:  return "InputAssembly";
    case CommandBufferD3DCallKind::InputAssemblySkip: return "InputAssemblySkip";
    case CommandBufferD3DCallKind::State:          return "State";
    case CommandBufferD3DCallKind::Draw:           return "Draw";
    case CommandBufferD3DCallKind::ResourceWait:   return "ResourceWait";
    case CommandBufferD3DCallKind::ResourceFlush:  return "ResourceFlush";
    case CommandBufferD3DCallKind::ResourceEscalate: return "ResourceEscalate";
    default:                                       return "?";
    }
}

struct DeferredDetailScopeEntry {
    int previousIndex = -1;
    int index = -1;
    std::chrono::steady_clock::time_point start;
    bool active = false;
};

thread_local DeferredDetailScopeEntry tl_deferredDetailStack[8];
thread_local int tl_deferredDetailDepth = 0;

inline void UpdateMax(std::atomic<std::uint64_t>& slot, std::uint64_t v) noexcept
{
    std::uint64_t prev = slot.load(std::memory_order_relaxed);
    while (v > prev && !slot.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
}

inline void RecordBucket(Bucket& b, std::uint64_t ns) noexcept
{
    b.calls.fetch_add(1, std::memory_order_relaxed);
    b.totalNs.fetch_add(ns, std::memory_order_relaxed);
    UpdateMax(b.maxNs, ns);
}

inline void IncrementDeferredDetailBSDraws() noexcept
{
    for (int i = 0; i < tl_deferredDetailDepth; ++i) {
        const auto& scope = tl_deferredDetailStack[i];
        if (!scope.active || scope.index < 0) continue;
        s_deferredDetails[scope.index / static_cast<int>(kDeferredDetailGroups)]
                         [scope.index % static_cast<int>(kDeferredDetailGroups)]
            .bucket.draws.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void IncrementDeferredDetailD3DDraws() noexcept
{
    for (int i = 0; i < tl_deferredDetailDepth; ++i) {
        const auto& scope = tl_deferredDetailStack[i];
        if (!scope.active || scope.index < 0) continue;
        s_deferredDetails[scope.index / static_cast<int>(kDeferredDetailGroups)]
                         [scope.index % static_cast<int>(kDeferredDetailGroups)]
            .bucket.d3dDraws.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void IncrementDeferredDetailCommandBufferDraws() noexcept
{
    for (int i = 0; i < tl_deferredDetailDepth; ++i) {
        const auto& scope = tl_deferredDetailStack[i];
        if (!scope.active || scope.index < 0) continue;
        s_deferredDetails[scope.index / static_cast<int>(kDeferredDetailGroups)]
                         [scope.index % static_cast<int>(kDeferredDetailGroups)]
            .bucket.cmdBufDraws.fetch_add(1, std::memory_order_relaxed);
    }
}

// --- Periodic logging ------------------------------------------------------

constexpr double kLogIntervalSecs = 2.0;
std::chrono::steady_clock::time_point s_lastLogTime;

std::atomic<bool> s_firstFrameLogged{ false };
std::atomic<bool> s_firstSubLogged[static_cast<std::size_t>(SubPhase::Count)] = {};

void MaybeLog()
{
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - s_lastLogTime).count();
    if (secs < kLogIntervalSecs) return;
    s_lastLogTime = now;

    const auto frameCalls = s_frame.calls.load(std::memory_order_relaxed);
    if (frameCalls == 0) return;

    const auto frameTotalNs = s_frame.totalNs.load(std::memory_order_relaxed);
    const auto frameDraws   = s_frame.draws.load(std::memory_order_relaxed);
    const auto frameD3DDraws = s_frame.d3dDraws.load(std::memory_order_relaxed);
    const auto frameCmdBufDraws = s_frame.cmdBufDraws.load(std::memory_order_relaxed);
    const auto frameMaxNs   = s_frame.maxNs.load(std::memory_order_relaxed);

    std::uint64_t childNsSum = 0;
    for (int i = 1; i < static_cast<int>(SubPhase::Count); ++i) {
        childNsSum += s_subBuckets[i].totalNs.load(std::memory_order_relaxed);
    }

    REX::INFO(
        "PhaseTelemetry[Frame]: calls={} over {:.2f}s ({:.1f}/s) "
        "totalMs/s={:.2f} avgMs={:.2f} maxMs={:.2f} bsDraws/frame={:.0f} cmdBuf/frame={:.0f} d3dDraws/frame={:.0f} "
        "unaccountedMs={:.2f}",
        frameCalls, secs, frameCalls / secs,
        (frameTotalNs / 1'000'000.0) / secs,
        (frameTotalNs / 1'000'000.0) / static_cast<double>(frameCalls),
        frameMaxNs / 1'000'000.0,
        static_cast<double>(frameDraws) / static_cast<double>(frameCalls),
        static_cast<double>(frameCmdBufDraws) / static_cast<double>(frameCalls),
        static_cast<double>(frameD3DDraws) / static_cast<double>(frameCalls),
        (frameTotalNs > childNsSum ? (frameTotalNs - childNsSum) : 0) / 1'000'000.0);

    for (int i = 1; i < static_cast<int>(SubPhase::Count); ++i) {
        const auto& b = s_subBuckets[i];
        const auto c = b.calls.load(std::memory_order_relaxed);
        if (c == 0) continue;
        const auto d  = b.draws.load(std::memory_order_relaxed);
        const auto dd = b.d3dDraws.load(std::memory_order_relaxed);
        const auto cd = b.cmdBufDraws.load(std::memory_order_relaxed);
        const auto ns = b.totalNs.load(std::memory_order_relaxed);
        const auto mx = b.maxNs.load(std::memory_order_relaxed);
        REX::INFO(
            "  Frame[{}]: calls={} bsDraws={} cmdBuf={} d3dDraws={} totMs/s={:.2f} avgUs={:.1f} maxUs={:.1f} "
            "(%frame={:.1f})",
            SubPhaseName(static_cast<SubPhase>(i)),
            c, d, cd, dd,
            (ns / 1'000'000.0) / secs,
            (ns / 1000.0) / static_cast<double>(c),
            mx / 1000.0,
            100.0 * static_cast<double>(ns) / static_cast<double>(frameTotalNs));
    }

    for (std::size_t kind = 0; kind < kDeferredDetailKinds; ++kind) {
        for (std::size_t group = 0; group < kDeferredDetailGroups; ++group) {
            const auto& detail = s_deferredDetails[kind][group];
            if (detail.active.load(std::memory_order_relaxed) == 0) {
                continue;
            }

            const auto& b = detail.bucket;
            const auto c = b.calls.load(std::memory_order_relaxed);
            if (c == 0) continue;
            const auto d  = b.draws.load(std::memory_order_relaxed);
            const auto dd = b.d3dDraws.load(std::memory_order_relaxed);
            const auto cd = b.cmdBufDraws.load(std::memory_order_relaxed);
            const auto ns = b.totalNs.load(std::memory_order_relaxed);
            const auto mx = b.maxNs.load(std::memory_order_relaxed);
            const auto detailKind = static_cast<DeferredPrePassDetailKind>(kind);
            const double pctDeferred =
                100.0 * static_cast<double>(ns) /
                std::max<double>(
                    1.0,
                    static_cast<double>(
                        s_subBuckets[static_cast<std::size_t>(SubPhase::DeferredPrePass)].totalNs.load(
                            std::memory_order_relaxed)));

            if (detailKind == DeferredPrePassDetailKind::RenderCommandBufferPasses && detail.metadataSet) {
                REX::INFO(
                    "    DeferredPrePass[{}:{}]: calls={} bsDraws={} cmdBuf={} d3dDraws={} d3d/call={:.1f} totMs/s={:.2f} avgUs={:.1f} maxUs={:.1f} (%deferred={:.1f}) chain={} head={} geom={} shader={} tech=0x{:08X} name='{}'",
                    DeferredDetailKindName(detailKind),
                    DeferredDetailKeyName(detailKind, group),
                    c, d, cd, dd,
                    static_cast<double>(dd) / static_cast<double>(c),
                    (ns / 1'000'000.0) / secs,
                    (ns / 1000.0) / static_cast<double>(c),
                    mx / 1000.0,
                    pctDeferred,
                    detail.chainLen,
                    detail.head,
                    detail.geometry,
                    detail.shader,
                    detail.techniqueID,
                    detail.geometryName);
            } else {
                REX::INFO(
                    "    DeferredPrePass[{}:{}]: calls={} bsDraws={} cmdBuf={} d3dDraws={} totMs/s={:.2f} avgUs={:.1f} maxUs={:.1f} (%deferred={:.1f})",
                    DeferredDetailKindName(detailKind),
                    DeferredDetailKeyName(detailKind, group),
                    c, d, cd, dd,
                    (ns / 1'000'000.0) / secs,
                    (ns / 1000.0) / static_cast<double>(c),
                    mx / 1000.0,
                    pctDeferred);
            }
        }
    }

    for (std::size_t kind = 0; kind < kCommandBufferD3DCallKinds; ++kind) {
        const auto& b = s_commandBufferD3DCalls[kind];
        const auto c = b.calls.load(std::memory_order_relaxed);
        if (c == 0) {
            continue;
        }

        const auto ns = b.totalNs.load(std::memory_order_relaxed);
        const auto mx = b.maxNs.load(std::memory_order_relaxed);
        REX::INFO(
            "    CommandBufferD3D[{}]: calls={} calls/frame={:.1f} totMs/s={:.2f} avgUs={:.2f} maxUs={:.1f}",
            CommandBufferD3DCallKindName(static_cast<CommandBufferD3DCallKind>(kind)),
            c,
            static_cast<double>(c) / static_cast<double>(frameCalls),
            (ns / 1'000'000.0) / secs,
            (ns / 1000.0) / static_cast<double>(c),
            mx / 1000.0);
    }

    s_frame.Reset();
    for (auto& b : s_subBuckets) b.Reset();
    for (auto& kind : s_deferredDetails) {
        for (auto& detail : kind) detail.Reset();
    }
    for (auto& b : s_commandBufferD3DCalls) b.Reset();
}

// --- Hook bodies ----------------------------------------------------------

void HookedRenderPreUI()
{
    const bool telemetryOn = g_mode.load(std::memory_order_relaxed) == Mode::On;
    if (!telemetryOn && !s_forceHooks.load(std::memory_order_relaxed)) {
        s_origRenderPreUI();
        return;
    }
    if (telemetryOn && !s_firstFrameLogged.exchange(true, std::memory_order_relaxed)) {
        REX::INFO("PhaseTelemetry: first DrawWorld::Render_PreUI call observed");
    }
    const bool wasInFrame = tl_inFrame;
    tl_inFrame = true;

    const auto t0 = std::chrono::steady_clock::now();
    s_origRenderPreUI();
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    tl_inFrame = wasInFrame;
    if (telemetryOn) {
        RecordBucket(s_frame, ns);

        MaybeLog();
    }
}

// Generic per-sub-phase body. Records into s_subBuckets[P] only if Render_PreUI
// is on the stack (tl_inFrame). Otherwise passthrough ??Render_PreUI must own
// the lifetime for our model to apply.
template <SubPhase P>
void HookedSubPhase(VoidVoid_t orig)
{
    const bool telemetryOn = g_mode.load(std::memory_order_relaxed) == Mode::On;
    if (!telemetryOn && !s_forceHooks.load(std::memory_order_relaxed)) {
        orig();
        return;
    }
    if (!tl_inFrame) {
        orig();
        return;
    }
    if (telemetryOn && !s_firstSubLogged[static_cast<std::size_t>(P)].exchange(true, std::memory_order_relaxed)) {
        REX::INFO("PhaseTelemetry: first DrawWorld::{} call observed", SubPhaseName(P));
    }
    const SubPhase prev = tl_subphase;
    tl_subphase = P;

    const auto t0 = std::chrono::steady_clock::now();
    orig();
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    tl_subphase = prev;
    if (telemetryOn) {
        RecordBucket(s_subBuckets[static_cast<std::size_t>(P)], ns);
    }
}

void HookedMainAccumCore()
{
    s_mainAccumActive.store(true, std::memory_order_release);
    s_origMainAccum();
    s_mainAccumActive.store(false, std::memory_order_release);
}
void HookedMainAccum()          { HookedSubPhase<SubPhase::MainAccum         >(&HookedMainAccumCore); }
void HookedMainRenderSetup()    { HookedSubPhase<SubPhase::MainRenderSetup   >(s_origMainRenderSetup); }
void HookedDeferredPrePassCore()
{
    s_origDeferredPrePass();
}
void HookedDeferredPrePass()    { HookedSubPhase<SubPhase::DeferredPrePass   >(&HookedDeferredPrePassCore); }
void HookedDeferredDecals()     { HookedSubPhase<SubPhase::DeferredDecals    >(s_origDeferredDecals); }
void HookedNvidiaHBAO()         { HookedSubPhase<SubPhase::NvidiaHBAO        >(s_origNvidiaHBAO); }
void HookedDeferredComposite()  { HookedSubPhase<SubPhase::DeferredComposite >(s_origDeferredComposite); }
void HookedForwardCore()
{
    s_origForward();
}
void HookedForward()            { HookedSubPhase<SubPhase::Forward           >(&HookedForwardCore); }
// Tier 0 ??installed via E9-5 + relocator.
void HookedLightUpdate()        { HookedSubPhase<SubPhase::LightUpdate       >(s_origLightUpdate); }
void HookedOpaqueWireframe()    { HookedSubPhase<SubPhase::OpaqueWireframe   >(s_origOpaqueWireframe); }
void HookedUpdateMotionBlur()   { HookedSubPhase<SubPhase::UpdateMotionBlur  >(s_origUpdateMotionBlur); }
void HookedImagespaceSAO()      { HookedSubPhase<SubPhase::ImagespaceSAO     >(s_origImagespaceSAO); }
void HookedRefraction()         { HookedSubPhase<SubPhase::Refraction        >(s_origRefraction); }

// --- Install helpers ------------------------------------------------------

enum class HookFlavor : std::uint8_t { E9_5, E9_5_Reloc };

bool InstallOne(const char* name, REL::Relocation<std::uintptr_t>& target,
                std::size_t prologueSize, HookFlavor flavor,
                void* hook, void** outOrig)
{
    const std::uintptr_t addr = target.address();

    const auto* original = reinterpret_cast<const std::uint8_t*>(addr);
    std::uint8_t captured[24] = {};
    const std::size_t capLen = std::min<std::size_t>(prologueSize, sizeof(captured));
    std::memcpy(captured, original, capLen);

    void* gateway = nullptr;
    if (flavor == HookFlavor::E9_5) {
        gateway = Hooks::CreateBranchGateway5<void*>(target, prologueSize, hook);
    } else {
        gateway = Hooks::CreateBranchGateway5Relocated<void*>(target, prologueSize, hook);
    }
    if (!gateway) {
        REX::WARN("PhaseTelemetry::Initialize: gateway install failed for {} @ {:#x} (flavor {})",
                  name, addr,
                  flavor == HookFlavor::E9_5 ? "E9-5"
                  : "E9-5+reloc (relocator rejected captured bytes)");
        return false;
    }
    *outOrig = gateway;

    std::string capStr;
    capStr.reserve(capLen * 3);
    for (std::size_t i = 0; i < capLen; ++i) {
        if (i) capStr.push_back(' ');
        char buf[3];
        std::snprintf(buf, sizeof(buf), "%02x", captured[i]);
        capStr.append(buf);
    }
    REX::INFO(
        "PhaseTelemetry::Initialize: hooked {} @ {:#x} (prologue={}B {} captured = {})",
        name, addr, prologueSize,
        flavor == HookFlavor::E9_5 ? "E9-5"
        : "E9-5+reloc",
        capStr);
    return true;
}

bool InstallCall(const char* name,
                 std::uintptr_t callSite,
                 std::uintptr_t expectedTarget,
                 void* hook,
                 void** outOrig)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(callSite);
    if (bytes[0] != 0xE8) {
        REX::WARN("PhaseTelemetry::Initialize: call-site install failed for {} @ {:#x}: expected E8, found {:02x}",
                  name, callSite, bytes[0]);
        return false;
    }

    REL::Relocation<std::uintptr_t> rel{ callSite };
    const auto original = rel.write_call<5>(reinterpret_cast<std::uintptr_t>(hook));
    *outOrig = reinterpret_cast<void*>(original);

    if (original != expectedTarget) {
        REX::WARN("PhaseTelemetry::Initialize: patched {} call site @ {:#x}, but original target was {:#x} (expected {:#x}); chaining current target",
                  name, callSite, original, expectedTarget);
    } else {
        REX::INFO("PhaseTelemetry::Initialize: patched {} call site @ {:#x} -> original {:#x}",
                  name, callSite, original);
    }
    return true;
}

bool s_installed = false;

}  // anonymous namespace

void OnDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) return;
    if (!tl_inFrame) return;
    s_frame.draws.fetch_add(1, std::memory_order_relaxed);
    if (tl_subphase != SubPhase::None) {
        s_subBuckets[static_cast<std::size_t>(tl_subphase)]
            .draws.fetch_add(1, std::memory_order_relaxed);
    }
    IncrementDeferredDetailBSDraws();
}

void OnD3DDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) return;
    if (!tl_inFrame) return;
    s_frame.d3dDraws.fetch_add(1, std::memory_order_relaxed);
    if (tl_subphase != SubPhase::None) {
        s_subBuckets[static_cast<std::size_t>(tl_subphase)]
            .d3dDraws.fetch_add(1, std::memory_order_relaxed);
    }
    IncrementDeferredDetailD3DDraws();
}

void OnCommandBufferDraw()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) return;
    if (!tl_inFrame) return;
    s_frame.cmdBufDraws.fetch_add(1, std::memory_order_relaxed);
    if (tl_subphase != SubPhase::None) {
        s_subBuckets[static_cast<std::size_t>(tl_subphase)]
            .cmdBufDraws.fetch_add(1, std::memory_order_relaxed);
    }
    IncrementDeferredDetailCommandBufferDraws();
}

void EnterCommandBufferReplay()
{
    ++tl_commandBufferReplayDepth;
}

void LeaveCommandBufferReplay()
{
    if (tl_commandBufferReplayDepth > 0) {
        --tl_commandBufferReplayDepth;
    }
}

bool IsInCommandBufferReplay()
{
    return tl_commandBufferReplayDepth != 0;
}

void OnCommandBufferD3DCall(CommandBufferD3DCallKind kind, std::uint64_t ns)
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On ||
        tl_commandBufferReplayDepth == 0 ||
        static_cast<std::size_t>(kind) >= kCommandBufferD3DCallKinds) {
        return;
    }

    RecordBucket(s_commandBufferD3DCalls[static_cast<std::size_t>(kind)], ns);
}

void RequireHooks()
{
    s_forceHooks.store(true, std::memory_order_relaxed);
}

bool IsInRenderPreUI()
{
    return tl_inFrame;
}

bool IsInMainAccum()
{
    return (tl_inFrame && tl_subphase == SubPhase::MainAccum) ||
           s_mainAccumActive.load(std::memory_order_acquire);
}

bool IsInDeferredPrePass()
{
    return tl_inFrame && tl_subphase == SubPhase::DeferredPrePass;
}

bool IsInDeferredLightsImpl()
{
    return tl_inFrame && tl_subphase == SubPhase::DeferredLightsImpl;
}

namespace {
struct DeferredLightsObserverScope {
    SubPhase previous = SubPhase::None;
    std::chrono::steady_clock::time_point start{};
    bool active = false;
    bool telemetry = false;
};

thread_local std::array<DeferredLightsObserverScope, 2> tl_deferredLightsObserverScopes{};
thread_local std::size_t tl_deferredLightsObserverDepth = 0;
thread_local std::size_t tl_deferredLightsObserverOverflowDepth = 0;
}  // namespace

void BeginDeferredLightsImpl()
{
    if (!tl_inFrame) {
        return;
    }
    if (tl_deferredLightsObserverDepth >= tl_deferredLightsObserverScopes.size()) {
        ++tl_deferredLightsObserverOverflowDepth;
        return;
    }

    auto& scope = tl_deferredLightsObserverScopes[tl_deferredLightsObserverDepth++];
    scope = {};
    scope.previous = tl_subphase;
    scope.telemetry = g_mode.load(std::memory_order_relaxed) == Mode::On;
    scope.active = true;
    tl_subphase = SubPhase::DeferredLightsImpl;

    if (scope.telemetry) {
        if (!s_firstSubLogged[static_cast<std::size_t>(SubPhase::DeferredLightsImpl)]
                 .exchange(true, std::memory_order_relaxed)) {
            REX::INFO("PhaseTelemetry: first DrawWorld::DeferredLightsImpl call observed");
        }
        scope.start = std::chrono::steady_clock::now();
    }
}

void EndDeferredLightsImpl()
{
    if (tl_deferredLightsObserverOverflowDepth > 0) {
        --tl_deferredLightsObserverOverflowDepth;
        return;
    }
    if (tl_deferredLightsObserverDepth == 0) {
        return;
    }

    auto& scope = tl_deferredLightsObserverScopes[--tl_deferredLightsObserverDepth];
    if (!scope.active) {
        return;
    }

    tl_subphase = scope.previous;
    if (scope.telemetry) {
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - scope.start)
                .count());
        RecordBucket(
            s_subBuckets[static_cast<std::size_t>(SubPhase::DeferredLightsImpl)],
            elapsed);
    }
    scope = {};
}

namespace {
int DeferredDetailIndex(DeferredPrePassDetailKind kind, std::uint32_t key)
{
    if (key >= kDeferredDetailGroups) {
        return -1;
    }
    return static_cast<int>(kind) * static_cast<int>(kDeferredDetailGroups) + static_cast<int>(key);
}
}

void BeginDeferredPrePassDetail(DeferredPrePassDetailKind kind, std::uint32_t key)
{
    if (tl_deferredDetailDepth >= static_cast<int>(std::size(tl_deferredDetailStack))) {
        return;
    }

    auto& scope = tl_deferredDetailStack[tl_deferredDetailDepth++];
    scope.previousIndex = tl_deferredDetailIndex;
    scope.index = -1;
    scope.active = false;
    scope.start = {};

    if (g_mode.load(std::memory_order_relaxed) != Mode::On || !IsInDeferredPrePass()) {
        return;
    }

    const int index = DeferredDetailIndex(kind, key);
    if (index < 0) {
        return;
    }

    auto& detail = s_deferredDetails[index / static_cast<int>(kDeferredDetailGroups)]
                                    [index % static_cast<int>(kDeferredDetailGroups)];
    detail.active.store(1, std::memory_order_relaxed);
    scope.index = index;
    scope.start = std::chrono::steady_clock::now();
    scope.active = true;
    tl_deferredDetailIndex = index;
}

void EndDeferredPrePassDetail(DeferredPrePassDetailKind kind, std::uint32_t key)
{
    if (tl_deferredDetailDepth <= 0) {
        return;
    }

    auto& scope = tl_deferredDetailStack[--tl_deferredDetailDepth];
    const int index = DeferredDetailIndex(kind, key);
    const auto t1 = std::chrono::steady_clock::now();
    if (scope.active && index >= 0 && index == scope.index) {
        auto& detail = s_deferredDetails[index / static_cast<int>(kDeferredDetailGroups)]
                                        [index % static_cast<int>(kDeferredDetailGroups)];
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - scope.start).count());
        RecordBucket(detail.bucket, ns);
    }

    tl_deferredDetailIndex = scope.previousIndex;
}

bool GetCurrentDeferredPrePassDetail(DeferredPrePassDetailKind& kind, std::uint32_t& key)
{
    if (tl_deferredDetailIndex < 0) {
        return false;
    }

    kind = static_cast<DeferredPrePassDetailKind>(
        tl_deferredDetailIndex / static_cast<int>(kDeferredDetailGroups));
    key = static_cast<std::uint32_t>(
        tl_deferredDetailIndex % static_cast<int>(kDeferredDetailGroups));
    return true;
}

void NoteDeferredPrePassCommandBuffer(
    std::uint32_t key,
    void* head,
    void* geometry,
    void* shader,
    std::uint32_t techniqueID,
    std::uint32_t chainLen,
    const char* geometryName)
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On || !IsInDeferredPrePass()) {
        return;
    }

    const int index = DeferredDetailIndex(DeferredPrePassDetailKind::RenderCommandBufferPasses, key);
    if (index < 0) {
        return;
    }

    auto& detail = s_deferredDetails[index / static_cast<int>(kDeferredDetailGroups)]
                                    [index % static_cast<int>(kDeferredDetailGroups)];
    if (detail.metadataSet) {
        return;
    }

    detail.head = head;
    detail.geometry = geometry;
    detail.shader = shader;
    detail.techniqueID = techniqueID;
    detail.chainLen = chainLen;
    detail.geometryName = geometryName ? geometryName : "";
    detail.metadataSet = true;
}

bool Initialize()
{
    if (s_installed) {
        REX::INFO("PhaseTelemetry::Initialize: already installed; skipping");
        return true;
    }
    const auto mode = g_mode.load(std::memory_order_relaxed);

    // Renderer features no longer piggy-back on these telemetry hooks.
    // ShadowUpgrade owns DeferredLightsImpl and reports it through observer
    // callbacks above.
    const bool forced = s_forceHooks.load(std::memory_order_relaxed);
    const bool needHooks = (mode == Mode::On) || forced;

    REX::INFO("PhaseTelemetry::Initialize: mode={} (hooks {}; telemetry={}, forced={})",
              mode == Mode::On ? "on" : "off",
              needHooks ? "installing" : "skipped",
              mode == Mode::On ? "on" : "off",
              forced ? "on" : "off");
    if (!needHooks) {
        return true;
    }

    s_lastLogTime = std::chrono::steady_clock::now();

    bool ok = true;
    ok &= InstallOne("DrawWorld::Render_PreUI",
                     ptr_RenderPreUI, kPrologueRenderPreUI, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedRenderPreUI),
                     reinterpret_cast<void**>(&s_origRenderPreUI));
    ok &= InstallOne("DrawWorld::MainAccum",
                     ptr_MainAccum, kPrologueMainAccum, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedMainAccum),
                     reinterpret_cast<void**>(&s_origMainAccum));
    ok &= InstallOne("DrawWorld::MainRenderSetup",
                     ptr_MainRenderSetup, kPrologueMainRenderSetup, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedMainRenderSetup),
                     reinterpret_cast<void**>(&s_origMainRenderSetup));
    ok &= InstallOne("DrawWorld::DeferredPrePass",
                     ptr_DeferredPrePass, kPrologueDeferredPrePass, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedDeferredPrePass),
                     reinterpret_cast<void**>(&s_origDeferredPrePass));
    ok &= InstallOne("DrawWorld::DeferredDecals",
                     ptr_DeferredDecals, kPrologueDeferredDecals, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedDeferredDecals),
                     reinterpret_cast<void**>(&s_origDeferredDecals));
    ok &= InstallOne("DrawWorld::NvidiaHBAO",
                     ptr_NvidiaHBAO, kPrologueNvidiaHBAO, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedNvidiaHBAO),
                     reinterpret_cast<void**>(&s_origNvidiaHBAO));
    ok &= InstallOne("DrawWorld::DeferredComposite",
                     ptr_DeferredComposite, kPrologueDeferredComposite, HookFlavor::E9_5,
                     reinterpret_cast<void*>(&HookedDeferredComposite),
                     reinterpret_cast<void**>(&s_origDeferredComposite));
    ok &= InstallCall("DrawWorld::Render_PreUI -> DrawWorld::Forward",
                      ptr_RenderPreUI.address() + kRenderPreUIForwardCallOffsetOG,
                      ptr_Forward.address(),
                      reinterpret_cast<void*>(&HookedForward),
                      reinterpret_cast<void**>(&s_origForward));

    // Tier 0 ??the five previously-skipped phases. All share a
    // `sub rsp,0x38; <rip-rel mov/cmp at byte 4>` prologue; we capture 11 B
    // and the relocator adjusts the disp32 of the rip-rel instruction.
    ok &= InstallOne("DrawWorld::LightUpdate",
                     ptr_LightUpdate, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedLightUpdate),
                     reinterpret_cast<void**>(&s_origLightUpdate));
    ok &= InstallOne("DrawWorld::OpaqueWireframe",
                     ptr_OpaqueWireframe, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedOpaqueWireframe),
                     reinterpret_cast<void**>(&s_origOpaqueWireframe));
    ok &= InstallOne("DrawWorld::UpdateMotionBlur",
                     ptr_UpdateMotionBlur, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedUpdateMotionBlur),
                     reinterpret_cast<void**>(&s_origUpdateMotionBlur));
    ok &= InstallOne("DrawWorld::ImagespaceSAO",
                     ptr_ImagespaceSAO, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedImagespaceSAO),
                     reinterpret_cast<void**>(&s_origImagespaceSAO));
    ok &= InstallOne("DrawWorld::Refraction",
                     ptr_Refraction, kPrologueTier0_SubAndRipRel, HookFlavor::E9_5_Reloc,
                     reinterpret_cast<void*>(&HookedRefraction),
                     reinterpret_cast<void**>(&s_origRefraction));

    if (!ok) {
        REX::WARN("PhaseTelemetry::Initialize: at least one hook failed; module remains partially installed (failed hooks: telemetry will skip those phases). All children gate on tl_inFrame, so if Render_PreUI itself failed, the module is effectively off.");
    }

    s_installed = true;
    REX::INFO("PhaseTelemetry::Initialize: installation complete; logging every {:.1f}s", kLogIntervalSecs);
    return ok;
}

}  // namespace PhaseTelemetry

#endif
