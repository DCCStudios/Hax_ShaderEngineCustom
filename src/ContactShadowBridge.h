#pragma once

#include <PCH.h>

// Publishes Bend Studio's screen-space-shadow dispatch list to the contact
// shadow compute pass as a structured SRV.
//
// Bend's CPU side (bend_sss_cpu.h, Apache-2.0, Graham Aldridge / Sony
// Interactive Entertainment) partitions the screen into up to eight wavefront
// volumes radiating from the light's screen position, each issued as its own
// Dispatch() with its own WaveOffset constant. ShaderEngine's custom-pass
// system issues one Dispatch with a fixed threadGroups= and has no per-pass
// constant buffer, so that structure cannot be expressed directly.
//
// Instead of building a bespoke C++ render pass - which would forfeit shader
// hot reload, GPU profiling and the whole custom-resource machinery - the eight
// dispatches are flattened into a single over-dispatched grid. This bridge
// computes the list, assigns each logical dispatch a contiguous range of flat
// group indices, and publishes the table. contactShadowRaymarch.hlsl maps its
// 2D group id to a flat index, finds the owning entry, and reconstructs the
// group id that entry's dispatch would have had.
//
// The grid width must match what CustomPass computes for `screenceil/N`, which
// resolves against renderTargets[kMain]. GridWidthFor() reproduces that exactly;
// if the two ever disagree the flattening silently addresses the wrong groups,
// so they are derived from the same source here rather than assumed.
namespace ContactShadowBridge
{
    // One past SunCascadeBridge's shadow array at t38.
    inline constexpr UINT SRV_SLOT = 39;

    // Bend's maximum: four quadrants, each of which may split once.
    inline constexpr UINT MAX_DISPATCHES = 8;

    // Must match BSS_WAVE_SIZE in the shader and numthreads(64,1,1).
    inline constexpr int WAVE_SIZE = 64;

    // Divisors in `threadGroups=screenceil/4,screenceil/8,1`. Changing either
    // here requires the same change in Shader.ini.
    inline constexpr UINT GRID_DIV_X = 4;
    inline constexpr UINT GRID_DIV_Y = 8;

    void Shutdown();

    // Rebuilds the dispatch list for this frame and binds it for a custom pass.
    // The custom-pass state snapshot restores the previous binding.
    void BindCustomPassResource(
        REX::W32::ID3D11DeviceContext* context,
        bool pixelStage);
}
