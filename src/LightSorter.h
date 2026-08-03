#pragma once

#include <atomic>
#include <cstdint>

// LightSorter — stable-partitions the point-light array on
// `ShadowSceneNode + 0x158` by the per-light stencil flag at `light + 0x17d`
// before DrawWorld::DeferredLightsImpl runs.
//
// Motivation (per Ghidra RE of DeferredLightsImpl @ 0x1428529B0):
// The point-light loop has two draw paths gated by cVar6 = light[0x17d]:
//
//   cVar6 == 0  → fast path: BSDFLightShader::SetupPointLightGeometry(...,uVar24)
//                            + BSBatchRenderer::Draw.  Sets uVar24 = 0.
//   cVar6 != 0  → slow path: BSBatchRenderer::EndPass + 5 render-state writes
//                            + Flush + RenderPassImmediately (×2) + EndPass.
//                            Sets uVar24 = 1.
//
// `uVar24` is the "fresh state" flag passed as the 4th arg to
// SetupPointLightGeometry. When 1, the shader rebuilds its CBs from scratch;
// when 0, it skips redundant CB writes. Each fast→slow transition resets it to
// 1; each slow→fast transition does too. Grouping all fast-path lights into a
// single run lets uVar24 collapse to 0 for the bulk of them.
//
// Strategy:
//   1. On DeferredLightsImpl entry: snapshot the array of N pointers into a
//      thread_local scratch buffer; stable-partition the live array so all
//      cVar6==0 entries come first.
//   2. On DeferredLightsImpl exit: memcpy the snapshot back to restore order.
//      This guards against any downstream consumer that depends on the
//      original (likely cull-order) sequence.
//
// Both hooks are no-ops when LIGHT_SORTER_MODE == off.

namespace LightSorter {

enum class Mode : std::uint8_t {
    Off,
    On,
};

extern std::atomic<Mode> g_mode;

// Called from ShadowUpgrade's validated DeferredLightsImpl wrapper at entry,
// before the original. Cheap (single relaxed load + branch) when mode == Off.
void OnEnter();

// Called from ShadowUpgrade's validated DeferredLightsImpl wrapper after the
// original returns. Restores the pre-sort pointer order so downstream code
// sees the unmodified array. No-op when mode == Off OR when OnEnter didn't sort
// (e.g. ShadowSceneNode pointer was null, or array was empty).
void OnExit();

void Initialize();

}  // namespace LightSorter
