#include <PCH.h>

#include "ShadowUpgrade.h"

#include <Global.h>
#include <LightSorter.h>
#include <PhaseTelemetry.h>
#include <RenderTargets.h>
#include <hooks.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>

// CustomPass.h is not standalone (it needs Plugin.h's types first), so the one
// entry point this file needs is declared rather than included. This MUST sit
// at global scope: inside ShadowUpgrade's anonymous namespace it declares a
// distinct, never-defined symbol and fails to link.
namespace CustomPass {
void FireAfterDeferredLightsPasses(REX::W32::ID3D11DeviceContext* context);
}

// Refreshes GFXInjected with the CURRENT frame's camera matrices (idempotent
// per frame). The draw-batch path calls this from Plugin.cpp so screen-space
// tracing sees the camera that produced the current depth; the
// afterDeferredLights path did NOT, so passes there (wetness, contact
// shadows) reconstructed world position with the PREVIOUS frame's camera and
// the whole field slid with the camera during a pan. Declared here (global
// scope) rather than including d3dhooks.h, matching the pattern above.
void RefreshCustomBufferForCustomPass();

namespace ShadowUpgrade {
namespace {

// IDA, Fallout 4 OG 1.10.163:
//   DrawWorld::DeferredLightsImpl @ 0x1428529B0, REL ID 1108521
//   ABI: static void(void); no register or stack arguments
//   safe prologue: 18 bytes, ending before the first preserved-register store
//   AE/NG: not confirmed, therefore deliberately unsupported.
REL::Relocation<std::uintptr_t> s_deferredLightsImpl{ REL::ID{ 1108521, 0 } };
constexpr std::array<std::uint8_t, 18> kDeferredLightsImplPrologue{
    0x48, 0x8B, 0xC4,                         // mov rax,rsp
    0x55,                                     // push rbp
    0x48, 0x8D, 0xA8, 0x88, 0xF9, 0xFF, 0xFF, // lea rbp,[rax-678h]
    0x48, 0x81, 0xEC, 0x70, 0x07, 0x00, 0x00  // sub rsp,770h
};

// IDA, Fallout 4 OG 1.10.163:
//   BSShadowDirectionalLight::UpdateCamerasI @ 0x1428CACC0
//   REL ID 1242204, ABI: bool __fastcall(this=RCX, camera=RDX)
//   0x1428CBA69 / function+0xDA9:
//     F3 44 0F 10 1D 86 87 42 00
//     movss xmm11, dword ptr [rip+0x428786] ; shared 800.0f
// This is a behavior-changing operand-only patch. The instruction and control
// flow remain intact; only its disp32 is redirected to private trampoline data.
// The source literal @ 0x142CF41F8 is not writable as configuration: IDA shows
// three unrelated AI/package references to the same 800.0f storage.
// AE/NG IDs and bytes are unconfirmed and deliberately unsupported.
REL::Relocation<std::uintptr_t> s_updateDirectionalCameras{
    REL::ID{ 1242204, 0 }
};
REL::Relocation<std::uintptr_t> s_liveDirectionalShadowDistance{
    REL::ID{ 777729, 0 }
};
constexpr std::ptrdiff_t kFirstCascadeLoadOffset = 0xDA9;
constexpr std::array<std::uint8_t, 9> kFirstCascadeLoad{
    0xF3, 0x44, 0x0F, 0x10, 0x1D, 0x86, 0x87, 0x42, 0x00
};
constexpr std::size_t kFirstCascadeDispOffset = 5;
constexpr float kVanillaSecondCascadeDistance = 3000.0f;

using DeferredLightsImpl_t = void (*)();
DeferredLightsImpl_t s_originalDeferredLightsImpl = nullptr;

// DrawWorld::DeferredComposite, REL ID 728427, E9-5, 15-byte prologue
// (mov rax,rsp + 2 saves + 5 pushes). Same target PhaseTelemetry hooks, but
// that module is compiled out by default (SHADERENGINE_ENABLE_PHASE_TELEMETRY
// is 0), so its wrapper never installs and cannot be relied on here.
//
// Why this hook exists at all: DeferredLightsImpl only ACCUMULATES lighting
// into the diffuse/specular buffers. DeferredComposite is what combines them
// into kMain. A pass firing after DeferredLightsImpl and writing kMain is
// therefore overwritten wholesale moments later - which is exactly what
// silently removed contact shadows when they were first moved off the tonemap
// trigger. After DeferredComposite is the real "lighting resolved, nothing
// transparent drawn yet" boundary.
REL::Relocation<std::uintptr_t> s_deferredComposite{ REL::ID{ 728427, 0 } };
constexpr std::size_t kDeferredCompositePrologue = 15;
using DeferredComposite_t = void (*)();
DeferredComposite_t s_originalDeferredComposite = nullptr;
float* s_firstCascadeDistanceStorage = nullptr;
bool s_firstCascadeDistanceInstalled = false;
bool s_installed = false;
bool s_installAttempted = false;
thread_local std::uint32_t s_deferredLightingDepth = 0;
thread_local std::uint32_t s_deferredReconstructionBindingDepth = 0;

struct ScopedDeferredLighting
{
    ScopedDeferredLighting() noexcept
    {
        ++s_deferredLightingDepth;
        PhaseTelemetry::BeginDeferredLightsImpl();
        LightSorter::OnEnter();
    }

    ~ScopedDeferredLighting()
    {
        LightSorter::OnExit();
        PhaseTelemetry::EndDeferredLightsImpl();
        if (s_deferredLightingDepth > 0) {
            --s_deferredLightingDepth;
        }
    }
};

// Snapshot of the per-frame constant buffer the deferred lights had bound at
// PS b12. Custom passes firing after DeferredComposite need the deferred
// lights' cb12 rows 20-27 (screen -> camera-relative world reconstruction,
// same space as the gbuffer normal - pixelDeferredLightOG.hlsl dots the
// decoded normal against normalize(-reconstructedPos) with no basis change),
// but by the time they fire, DeferredComposite has drawn with its OWN b12
// binding, so "read whatever is at b12" sampled the wrong buffer. The copy is
// taken with CopyResource on the GPU timeline immediately after the light
// draws, freezing exactly the contents the lights read even if the engine
// rewrites that buffer later in the frame.
struct LightsCBSnapshot
{
    UINT slot = 0;
    REX::W32::ID3D11Buffer* copy = nullptr;
    UINT copySize = 0;
    bool valid = false;
    bool warnedMissing = false;

    void Capture(REX::W32::ID3D11DeviceContext* context,
                 REX::W32::ID3D11Device* device)
    {
        REX::W32::ID3D11Buffer* source = nullptr;
        context->PSGetConstantBuffers(slot, 1, &source);
        if (!source) {
            if (!warnedMissing) {
                warnedMissing = true;
                REX::WARN(
                    "ShadowUpgrade: no PS b{} bound at end of "
                    "DeferredLightsImpl; afterDeferredLights passes get no "
                    "engine cb{} snapshot",
                    slot, slot);
            }
            valid = false;
            return;
        }

        REX::W32::D3D11_BUFFER_DESC sourceDesc{};
        source->GetDesc(&sourceDesc);
        if (copy && copySize != sourceDesc.byteWidth) {
            copy->Release();
            copy = nullptr;
        }
        if (!copy) {
            REX::W32::D3D11_BUFFER_DESC copyDesc{};
            copyDesc.byteWidth           = sourceDesc.byteWidth;
            copyDesc.usage               = REX::W32::D3D11_USAGE_DEFAULT;
            copyDesc.bindFlags           = REX::W32::D3D11_BIND_CONSTANT_BUFFER;
            copyDesc.cpuAccessFlags      = 0;
            copyDesc.miscFlags           = 0;
            copyDesc.structureByteStride = 0;
            const HRESULT hr = device->CreateBuffer(&copyDesc, nullptr, &copy);
            if (FAILED(hr) || !copy) {
                copy = nullptr;
                valid = false;
                source->Release();
                return;
            }
            copySize = sourceDesc.byteWidth;
            REX::INFO(
                "ShadowUpgrade: deferred-lights cb{} snapshot buffer created "
                "({} bytes = {} float4 rows)",
                slot, sourceDesc.byteWidth, sourceDesc.byteWidth / 16);
        }

        context->CopyResource(copy, source);
        valid = true;
        source->Release();
    }
};

// cb12 = the reconstruction rows (20-27); cb2 = the lights' per-draw scale
// factors (row 0 = 1/renderSize for clip coords, row 27 = dynamic-res subrect
// scale) plus shadow matrices and sun direction. Both are needed for a fully
// engine-verbatim world reconstruction: cb12 alone fixed the matrix content
// but the clip normalization still came from g_RenderInfo, which can disagree
// with the actual light-pass extent under dynamic resolution - a screen-scale
// error that makes the reconstructed field slide during camera rotation.
LightsCBSnapshot s_lightsCB2Snapshot{ 2 };
LightsCBSnapshot s_lightsCB12Snapshot{ 12 };

void CaptureDeferredLightsConstants()
{
    if (!g_rendererData || !g_rendererData->context || !g_rendererData->device) {
        return;
    }
    s_lightsCB2Snapshot.Capture(g_rendererData->context, g_rendererData->device);
    s_lightsCB12Snapshot.Capture(g_rendererData->context, g_rendererData->device);
}

void HookedDeferredLightsImpl()
{
    {
        ScopedDeferredLighting scope;
        s_originalDeferredLightsImpl();
    }
    CaptureDeferredLightsConstants();
}

void HookedDeferredComposite()
{
    s_originalDeferredComposite();

    // The scene now holds the composited lit opaques and nothing transparent
    // has drawn yet. Passes that multiply into the scene fire here so they
    // darken lit opaque surfaces only - particles, smoke, glass and fog blend
    // over the shadowed result afterwards instead of being darkened by it.
    //
    // MAIN SCENE ONLY, by IDENTITY. The engine runs this same composite with
    // more than one target bound across frames: confirmed in game 2026-08-14
    // as a 2560x1440 target (2/3 of 3840x2160 - an upscaler Quality-mode
    // proxy) alternating with the display-sized one in ~0.5s blocks at
    // GPU-heavy camera angles. A size-based gate ("must match the kMain
    // allocation") still lost shadows there: when the upscaler proxy-swaps
    // kMain itself, the smaller target IS the main scene, and the
    // display-sized composite of those frames is overwritten by the upscale.
    // The one stable truth is renderTargets[kMain].texture - the engine and
    // every downstream pass read the scene through that slot, so fire exactly
    // when the bound target is that texture, whatever size it currently is.
    if (!g_rendererData || !g_rendererData->context) {
        return;
    }
    auto* context = g_rendererData->context;

    bool mainSceneTarget = false;
    REX::W32::ID3D11Texture2D* boundTexture = nullptr;
    std::uint32_t boundW = 0, boundH = 0;
    REX::W32::ID3D11RenderTargetView* rtv0 = nullptr;
    context->OMGetRenderTargets(1, &rtv0, nullptr);
    if (rtv0) {
        REX::W32::ID3D11Resource* resource = nullptr;
        rtv0->GetResource(&resource);
        if (resource) {
            resource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                     reinterpret_cast<void**>(&boundTexture));
            if (boundTexture) {
                REX::W32::D3D11_TEXTURE2D_DESC boundDesc{};
                boundTexture->GetDesc(&boundDesc);
                boundW = boundDesc.width;
                boundH = boundDesc.height;
            }
            resource->Release();
        }
        rtv0->Release();
    }

    // Identity against renderTargets[kMain] was tried and is WRONG: in-game
    // 2026-08-14 the composite's destination is never that texture. It writes
    // one of two un-tabled buffers - display-sized (0x...d0e60-style) or a
    // 2560x1440 upscaler Quality proxy - while kMain is a third texture the
    // composite reads as an input. There is no stable identity to gate on, so
    // fire for every plausible scene-sized composite and let the passes bind
    // whatever target is live; the composite shader derives its extents from
    // the bound target, so each fire is self-consistent.
    auto* mainTexture =
        g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture;
    mainSceneTarget = boundTexture && boundW >= 16 && boundH >= 16;

    // Per-call transition diagnostic (capped 4/s): bound target vs kMain
    // identity across composite calls. This is what distinguishes "kMain was
    // proxy-swapped" from "a separate view rendered" the next time the
    // frame structure surprises us. Pointer values are compared, never
    // dereferenced after release.
    {
        static const void* s_lastBound = reinterpret_cast<const void*>(~uintptr_t{0});
        static const void* s_lastMain = nullptr;
        static std::uint64_t s_lastLogMs = 0;
        if (boundTexture != s_lastBound || mainTexture != s_lastMain) {
            s_lastBound = boundTexture;
            s_lastMain = mainTexture;
            const auto nowMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (nowMs - s_lastLogMs > 250) {
                s_lastLogMs = nowMs;
                REX::INFO(
                    "DeferredComposite: bound={} ({}x{}) kMain={} fire={}",
                    static_cast<const void*>(boundTexture), boundW, boundH,
                    static_cast<const void*>(mainTexture),
                    mainSceneTarget ? 1 : 0);
            }
        }
    }

    if (boundTexture) {
        boundTexture->Release();
    }

    if (mainSceneTarget) {
        // Give the afterDeferredLights passes the current frame's camera
        // matrices (matching this frame's depth) before they reconstruct
        // world position. Idempotent per frame: a no-op if a draw-batch pass
        // already refreshed earlier this frame.
        ::RefreshCustomBufferForCustomPass();
        // Bind the deferred lights' cb2/cb12 snapshots for the batch. The
        // composite draw that just ran left its OWN bindings there, so
        // without this a pass declaring `cbuffer cb12 : register(b12)` (or
        // cb2) reads the composite's buffers, not the reconstruction rows
        // and clip scale factors the lights used. The pass executor never
        // touches PS constant buffers, so the bindings hold for every pass
        // in the batch.
        REX::W32::ID3D11Buffer* previousB2 = nullptr;
        REX::W32::ID3D11Buffer* previousB12 = nullptr;
        context->PSGetConstantBuffers(2, 1, &previousB2);
        context->PSGetConstantBuffers(12, 1, &previousB12);
        if (s_lightsCB2Snapshot.valid && s_lightsCB2Snapshot.copy) {
            context->PSSetConstantBuffers(2, 1, &s_lightsCB2Snapshot.copy);
        }
        if (s_lightsCB12Snapshot.valid && s_lightsCB12Snapshot.copy) {
            context->PSSetConstantBuffers(12, 1, &s_lightsCB12Snapshot.copy);
        }
        ::CustomPass::FireAfterDeferredLightsPasses(context);
        context->PSSetConstantBuffers(2, 1, &previousB2);
        context->PSSetConstantBuffers(12, 1, &previousB12);
        if (previousB2) {
            previousB2->Release();
        }
        if (previousB12) {
            previousB12->Release();
        }
    }
}

bool IsAddressInText(std::uintptr_t address, std::size_t byteCount) noexcept
{
    const auto module = REX::FModule::GetExecutingModule();
    const auto text = module.GetSection(".text");
    const auto begin = text.GetAddress();
    const auto end = begin + text.GetSize();
    return begin != 0 && address >= begin && address <= end &&
        byteCount <= end - address;
}

bool IsAddressInData(std::uintptr_t address, std::size_t byteCount) noexcept
{
    const auto module = REX::FModule::GetExecutingModule();
    const auto data = module.GetSection(".data");
    const auto begin = data.GetAddress();
    const auto end = begin + data.GetSize();
    return begin != 0 && address >= begin && address <= end &&
        byteCount <= end - address;
}

bool InstallFirstCascadeDistanceOverride()
{
    const auto loadAddress =
        s_updateDirectionalCameras.address() + kFirstCascadeLoadOffset;
    if (!IsAddressInText(loadAddress, kFirstCascadeLoad.size())) {
        REX::WARN(
            "ShadowUpgrade: first-cascade load address {:#x} is outside "
            "Fallout4.exe .text; override not installed",
            loadAddress);
        return false;
    }
    if (std::memcmp(
            reinterpret_cast<const void*>(loadAddress),
            kFirstCascadeLoad.data(),
            kFirstCascadeLoad.size()) != 0) {
        REX::WARN(
            "ShadowUpgrade: BSShadowDirectionalLight::UpdateCamerasI+0x{:X} "
            "failed expected-byte validation; first-cascade override not "
            "installed",
            kFirstCascadeLoadOffset);
        return false;
    }

    const auto liveDistanceAddress = s_liveDirectionalShadowDistance.address();
    if (!IsAddressInData(liveDistanceAddress, sizeof(float))) {
        REX::WARN(
            "ShadowUpgrade: live fDirShadowDistance address {:#x} is outside "
            "Fallout4.exe .data; first-cascade override not installed",
            liveDistanceAddress);
        return false;
    }

    float liveDistance = 0.0f;
    std::memcpy(
        &liveDistance,
        reinterpret_cast<const void*>(liveDistanceAddress),
        sizeof(liveDistance));
    const auto configuredDistance =
        DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE;
    if (!std::isfinite(configuredDistance) || configuredDistance <= 0.0f ||
        configuredDistance >= kVanillaSecondCascadeDistance) {
        REX::WARN(
            "ShadowUpgrade: first-cascade distance {} rejected; it must be "
            "finite, positive, and below the vanilla second split ({}). "
            "Vanilla 800 remains active",
            configuredDistance,
            kVanillaSecondCascadeDistance);
        return false;
    }
    if (!std::isfinite(liveDistance) || liveDistance <= 0.0f) {
        REX::INFO(
            "ShadowUpgrade: live fDirShadowDistance is not populated yet "
            "({}); first-cascade override deferred",
            liveDistance);
        return false;
    }
    if (configuredDistance >= liveDistance) {
        REX::WARN(
            "ShadowUpgrade: first-cascade distance {} rejected because it is "
            "not below live fDirShadowDistance ({}). Vanilla 800 remains "
            "active",
            configuredDistance,
            liveDistance);
        return false;
    }

    auto& trampoline = REL::GetTrampoline();
    constexpr auto storageBytes =
        sizeof(float) + alignof(float) - 1;
    if (trampoline.free_size() < storageBytes) {
        REX::WARN(
            "ShadowUpgrade: insufficient trampoline space for private "
            "first-cascade data; override not installed");
        return false;
    }

    auto* rawStorage =
        static_cast<std::byte*>(trampoline.allocate(storageBytes));
    const auto alignedStorageAddress =
        (reinterpret_cast<std::uintptr_t>(rawStorage) + alignof(float) - 1) &
        ~(static_cast<std::uintptr_t>(alignof(float)) - 1);
    s_firstCascadeDistanceStorage =
        reinterpret_cast<float*>(alignedStorageAddress);
    std::memcpy(
        s_firstCascadeDistanceStorage,
        &configuredDistance,
        sizeof(configuredDistance));

    const auto instructionEnd = loadAddress + kFirstCascadeLoad.size();
    const auto displacement64 =
        static_cast<std::int64_t>(alignedStorageAddress) -
        static_cast<std::int64_t>(instructionEnd);
    if (displacement64 < (std::numeric_limits<std::int32_t>::min)() ||
        displacement64 > (std::numeric_limits<std::int32_t>::max)()) {
        REX::WARN(
            "ShadowUpgrade: private first-cascade data @ {:#x} is outside "
            "RIP-relative disp32 range of load @ {:#x}; override not installed",
            alignedStorageAddress,
            loadAddress);
        return false;
    }

    const auto displacement = static_cast<std::int32_t>(displacement64);
    if (!REL::WriteSafeData(
            loadAddress + kFirstCascadeDispOffset,
            displacement)) {
        REX::WARN(
            "ShadowUpgrade: failed to redirect first-cascade load operand; "
            "override not installed");
        return false;
    }
    ::FlushInstructionCache(
        ::GetCurrentProcess(),
        reinterpret_cast<const void*>(loadAddress),
        kFirstCascadeLoad.size());

    REX::INFO(
        "ShadowUpgrade: directional first-cascade distance set to {} "
        "(vanilla 800, live fDirShadowDistance={}); restart required for INI "
        "changes",
        configuredDistance,
        liveDistance);
    s_firstCascadeDistanceInstalled = true;
    return true;
}

}  // namespace

bool BindDeferredReconstructionConstants(
    REX::W32::ID3D11DeviceContext* context,
    DeferredReconstructionBinding& binding) noexcept
{
    binding = {};
    if (!context) {
        return false;
    }
    const bool b2Ready =
        s_lightsCB2Snapshot.valid && s_lightsCB2Snapshot.copy;
    const bool b12Ready =
        s_lightsCB12Snapshot.valid && s_lightsCB12Snapshot.copy;
    if (!b2Ready || !b12Ready) {
        static std::atomic_bool s_warnedMissingLateConstants{false};
        if (!s_warnedMissingLateConstants.exchange(
                true, std::memory_order_relaxed)) {
            REX::WARN(
                "ShadowUpgrade: late visualTonemap custom PS batch requested "
                "DeferredLights reconstruction before both snapshots existed "
                "(b2Ready={} b12Ready={}); viewmodel contact trace will fail "
                "open",
                b2Ready, b12Ready);
        }
        return false;
    }

    context->PSGetConstantBuffers(2, 1, &binding.previousB2);
    context->PSGetConstantBuffers(12, 1, &binding.previousB12);
    context->PSSetConstantBuffers(2, 1, &s_lightsCB2Snapshot.copy);
    context->PSSetConstantBuffers(12, 1, &s_lightsCB12Snapshot.copy);
    binding.active = true;
    ++s_deferredReconstructionBindingDepth;

    static std::atomic_bool s_loggedLateConstantsBind{false};
    if (!s_loggedLateConstantsBind.exchange(
            true, std::memory_order_relaxed)) {
        REX::INFO(
            "ShadowUpgrade: publishing DeferredLights b2+b12 snapshots to late "
            "visualTonemap custom PS batches ({} + {} bytes)",
            s_lightsCB2Snapshot.copySize,
            s_lightsCB12Snapshot.copySize);
    }
    return true;
}

void RestoreDeferredReconstructionConstants(
    REX::W32::ID3D11DeviceContext* context,
    DeferredReconstructionBinding& binding) noexcept
{
    if (!binding.active) {
        return;
    }
    if (context) {
        context->PSSetConstantBuffers(2, 1, &binding.previousB2);
        context->PSSetConstantBuffers(12, 1, &binding.previousB12);
    }
    if (binding.previousB2) {
        binding.previousB2->Release();
    }
    if (binding.previousB12) {
        binding.previousB12->Release();
    }
    binding = {};
    if (s_deferredReconstructionBindingDepth > 0) {
        --s_deferredReconstructionBindingDepth;
    }
}

bool IsDeferredReconstructionBoundForCurrentThread() noexcept
{
    return s_deferredReconstructionBindingDepth > 0;
}

bool Initialize()
{
    if (s_installed) {
        return true;
    }
    if (s_installAttempted) {
        return false;
    }
    s_installAttempted = true;

    if (!REX::FModule::IsRuntimeOG()) {
        REX::WARN(
            "ShadowUpgrade::Initialize: OG 1.10.163 is the only "
            "IDA-confirmed runtime; hook not installed");
        return false;
    }

    const auto runtime = REX::FModule::GetExecutingModule().GetFileVersion();
    if (runtime != REL::Version{ 1, 10, 163, 0 }) {
        REX::WARN(
            "ShadowUpgrade::Initialize: executable is {} rather than "
            "1.10.163.0; hook not installed",
            runtime.string());
        return false;
    }

    const auto address = s_deferredLightsImpl.address();
    if (!IsAddressInText(address, kDeferredLightsImplPrologue.size())) {
        REX::WARN(
            "ShadowUpgrade::Initialize: DeferredLightsImpl address {:#x} is "
            "outside Fallout4.exe .text",
            address);
        return false;
    }
    if (std::memcmp(
            reinterpret_cast<const void*>(address),
            kDeferredLightsImplPrologue.data(),
            kDeferredLightsImplPrologue.size()) != 0) {
        REX::WARN(
            "ShadowUpgrade::Initialize: DeferredLightsImpl @ {:#x} failed "
            "expected-byte validation; no patch applied",
            address);
        return false;
    }

    s_originalDeferredLightsImpl =
        Hooks::CreateBranchGateway5<DeferredLightsImpl_t>(
            s_deferredLightsImpl,
            kDeferredLightsImplPrologue.size(),
            reinterpret_cast<void*>(&HookedDeferredLightsImpl));
    if (!s_originalDeferredLightsImpl) {
        REX::WARN(
            "ShadowUpgrade::Initialize: failed to create DeferredLightsImpl gateway");
        return false;
    }

    const auto compositeAddress = s_deferredComposite.address();
    if (IsAddressInText(compositeAddress, kDeferredCompositePrologue)) {
        s_originalDeferredComposite =
            Hooks::CreateBranchGateway5<DeferredComposite_t>(
                s_deferredComposite,
                kDeferredCompositePrologue,
                reinterpret_cast<void*>(&HookedDeferredComposite));
    }
    if (!s_originalDeferredComposite) {
        // Non-fatal: only the afterDeferredLights trigger is lost, and passes
        // using it simply never fire rather than firing at the wrong time.
        REX::WARN(
            "ShadowUpgrade::Initialize: DeferredComposite wrapper NOT installed "
            "at {:#x}; afterDeferredLights passes will not fire",
            compositeAddress);
    } else {
        REX::INFO(
            "ShadowUpgrade::Initialize: DeferredComposite wrapper installed "
            "at {:#x} (afterDeferredLights trigger live)",
            compositeAddress);
    }

    s_installed = true;
    REX::INFO(
        "ShadowUpgrade::Initialize: renderer-phase wrapper installed at {:#x}; "
        "shader permutations remain on the normal ShaderEngine path",
        address);
    return true;
}

void OnGameDataReady()
{
    if (!SHADOW_UPGRADE_ON || s_firstCascadeDistanceInstalled) {
        return;
    }
    if (!REX::FModule::IsRuntimeOG() ||
        REX::FModule::GetExecutingModule().GetFileVersion() !=
            REL::Version{ 1, 10, 163, 0 }) {
        return;
    }

    InstallFirstCascadeDistanceOverride();
}

bool IsInDeferredLighting() noexcept
{
    return s_installed && s_deferredLightingDepth > 0;
}

}  // namespace ShadowUpgrade
