#include <PCH.h>

#include "ShadowUpgrade.h"

#include <Global.h>
#include <LightSorter.h>
#include <PhaseTelemetry.h>
#include <hooks.h>

#include <array>
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

void HookedDeferredLightsImpl()
{
    ScopedDeferredLighting scope;
    s_originalDeferredLightsImpl();
}

void HookedDeferredComposite()
{
    s_originalDeferredComposite();

    // kMain now holds the composited lit scene and nothing transparent has
    // drawn yet. Passes that multiply into the scene fire here so they darken
    // lit opaque surfaces only - particles, smoke, glass and fog blend over the
    // shadowed result afterwards instead of being darkened by it.
    if (g_rendererData && g_rendererData->context) {
        ::CustomPass::FireAfterDeferredLightsPasses(g_rendererData->context);
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
