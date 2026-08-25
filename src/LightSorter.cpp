#include <PCH.h>
#include "LightSorter.h"

#include <cstring>

namespace LightSorter {

std::atomic<Mode> g_mode{ Mode::Off };

namespace {

// --- Engine layout (verified via Ghidra) -----------------------------------
//
// DAT_1467231b0 is the OG global ShadowSceneNode* used by DrawWorld::DeferredLightsImpl
// for point-light iteration. Its layout (offsets cross-checked against the
// ShadowSceneNode::GetLight @ 0x1428117e0 single-line implementation):
//
//   ShadowSceneNode*
//     +0x158   BSLight**  pointPtrs       — array of point-light pointers
//     +0x168   uint32_t   pointCount      — number of valid entries
//     +0x170   BSLight**  shadowPtrs      — array of shadow-light pointers
//     +0x180   uint32_t   shadowCount
//
// Per-light field of interest:
//   BSLight*
//     +0x17d   uint8_t    stencilFlag     — cVar6 in the DeferredLightsImpl
//                                            decompile; selects fast/slow path
REL::Relocation<void**> ptr_ShadowSceneNode{ REL::ID{ 879298, 0 } };
constexpr std::size_t    kSSN_PointPtrsOffset  = 0x158;
constexpr std::size_t    kSSN_PointCountOffset = 0x168;
constexpr std::size_t    kSSN_ShadowPtrsOffset = 0x170;
constexpr std::size_t    kSSN_ShadowCountOffset = 0x180;
constexpr std::size_t    kLight_StencilFlagOff = 0x17d;

// Maximum number of point lights we can save/restore. Boston gameplay shows
// ~100-140 active; 256 leaves headroom. Buffer is thread_local so it costs
// 2 KB once per render thread that ever calls OnEnter.
constexpr std::size_t kMaxLightsTracked = 256;

// thread_local because DeferredLightsImpl runs on the render thread but TLS
// keeps us safe if any worker path ever entered.
thread_local void*       tl_saved[kMaxLightsTracked];
thread_local std::size_t tl_savedCount     = 0;
thread_local void**      tl_savedArrayBase = nullptr;  // where to restore

// Read the ShadowSceneNode pointer through the global. Returns nullptr if
// the global slot is itself null (rare — happens during init/teardown).
inline void* ReadShadowSceneNode() noexcept
{
    void* ssn = nullptr;
    const auto slot = ptr_ShadowSceneNode.get();
    std::memcpy(&ssn, slot, sizeof(ssn));
    return ssn;
}

// In-place stable partition: all entries with stencilFlag==0 first, then all
// entries with stencilFlag!=0. Order within each group is preserved (matches
// the original cull order, so the fast/slow runs stay deterministic).
void StablePartition(void** arr, std::size_t count) noexcept
{
    // Two-pass: scratch holds slow-path pointers in original order; we then
    // compact fast-path in place and append slow tail from scratch.
    void* slow[kMaxLightsTracked];
    std::size_t slowN = 0;
    std::size_t fastWrite = 0;
    for (std::size_t i = 0; i < count; ++i) {
        void* p = arr[i];
        if (!p) {
            // Preserve nulls in their relative position with fast-path entries
            // — they don't take either path, but the engine's visibility test
            // short-circuits on light[+0x18]==0xff anyway. Treat as fast.
            arr[fastWrite++] = nullptr;
            continue;
        }
        std::uint8_t flag = 0;
        std::memcpy(&flag, static_cast<std::uint8_t*>(p) + kLight_StencilFlagOff, 1);
        if (flag == 0) {
            arr[fastWrite++] = p;
        } else {
            slow[slowN++] = p;
        }
    }
    if (slowN > 0) {
        std::memcpy(arr + fastWrite, slow, slowN * sizeof(void*));
    }
}

}  // anonymous namespace

void OnEnter()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) return;

    tl_savedCount     = 0;
    tl_savedArrayBase = nullptr;

    void* ssn = ReadShadowSceneNode();
    if (!ssn) return;

    void** arr = nullptr;
    std::uint32_t count = 0;
    std::memcpy(&arr,   static_cast<std::uint8_t*>(ssn) + kSSN_PointPtrsOffset,  sizeof(arr));
    std::memcpy(&count, static_cast<std::uint8_t*>(ssn) + kSSN_PointCountOffset, sizeof(count));
    if (!arr || count == 0 || count > kMaxLightsTracked) return;

    // Save originals into TLS scratch before mutating.
    std::memcpy(tl_saved, arr, count * sizeof(void*));
    tl_savedCount     = count;
    tl_savedArrayBase = arr;

    StablePartition(arr, count);
}

void OnExit()
{
    if (g_mode.load(std::memory_order_relaxed) != Mode::On) return;
    if (tl_savedCount == 0 || tl_savedArrayBase == nullptr) return;
    // Restore original order. The light array's contents are unchanged between
    // OnEnter and OnExit because DeferredLightsImpl iterates without
    // re-querying the count or rebuilding the array; the only ShadowSceneNode
    // mutation it does is per-light state, not the index ordering.
    std::memcpy(tl_savedArrayBase, tl_saved, tl_savedCount * sizeof(void*));
    tl_savedCount     = 0;
    tl_savedArrayBase = nullptr;
}

bool IsShadowMappedLight(const void* light) noexcept
{
    if (!light || !REX::FModule::IsRuntimeOG()) return true;

    void* ssn = ReadShadowSceneNode();
    if (!ssn) return true;

    void** shadowLights = nullptr;
    std::uint32_t shadowCount = 0;
    std::memcpy(
        &shadowLights,
        static_cast<std::uint8_t*>(ssn) + kSSN_ShadowPtrsOffset,
        sizeof(shadowLights));
    std::memcpy(
        &shadowCount,
        static_cast<std::uint8_t*>(ssn) + kSSN_ShadowCountOffset,
        sizeof(shadowCount));
    if (shadowCount == 0) return false;
    // The engine's tiled light contract tops out at 625 records. Treat a
    // malformed pointer or count conservatively so we never double darken an
    // unknown light as a side effect of bad classification data.
    if (!shadowLights || shadowCount > 625) return true;
    return std::find(shadowLights, shadowLights + shadowCount, light) !=
        shadowLights + shadowCount;
}

std::uint32_t GetShadowLightCount() noexcept
{
    if (!REX::FModule::IsRuntimeOG()) return 0;

    void* ssn = ReadShadowSceneNode();
    if (!ssn) return 0;

    void** shadowLights = nullptr;
    std::uint32_t shadowCount = 0;
    std::memcpy(
        &shadowLights,
        static_cast<std::uint8_t*>(ssn) + kSSN_ShadowPtrsOffset,
        sizeof(shadowLights));
    std::memcpy(
        &shadowCount,
        static_cast<std::uint8_t*>(ssn) + kSSN_ShadowCountOffset,
        sizeof(shadowCount));
    return shadowLights && shadowCount <= 625 ? shadowCount : 0;
}

std::size_t CopyShadowLightPointers(
    void** destination,
    std::size_t capacity) noexcept
{
    if (!destination || capacity == 0 || !REX::FModule::IsRuntimeOG()) {
        return 0;
    }

    void* ssn = ReadShadowSceneNode();
    if (!ssn) return 0;

    void** shadowLights = nullptr;
    std::uint32_t shadowCount = 0;
    std::memcpy(
        &shadowLights,
        static_cast<std::uint8_t*>(ssn) + kSSN_ShadowPtrsOffset,
        sizeof(shadowLights));
    std::memcpy(
        &shadowCount,
        static_cast<std::uint8_t*>(ssn) + kSSN_ShadowCountOffset,
        sizeof(shadowCount));
    if (!shadowLights || shadowCount == 0 || shadowCount > 625) return 0;

    const std::size_t copied = (std::min)(
        capacity,
        static_cast<std::size_t>(shadowCount));
    std::memcpy(destination, shadowLights, copied * sizeof(void*));
    return copied;
}

void Initialize()
{
    // No log — this module is silent by design now. Mode is read from the
    // global atomic on every OnEnter/OnExit call.
}

}  // namespace LightSorter
