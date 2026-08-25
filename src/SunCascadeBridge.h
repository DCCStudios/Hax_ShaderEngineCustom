#pragma once

#include <PCH.h>

// Publishes the sun's per-cascade shadow transforms to custom passes as a
// structured SRV.
//
// This deliberately does NOT extend GFXInjected. That struct is read by every
// replacement and custom-pass shader, and growing it by the 13 float4s this
// data needs enlarged the shared structured-buffer stride enough to push FXC's
// loop-unroll heuristic past its budget in visualDOFAutoFocus. That shader
// stopped compiling, its pass was dropped, and the frame went black. Cascade
// data is wanted by exactly one pass, so it lives in its own buffer at its own
// slot and costs every other shader nothing.
//
// Source data comes from ShadowTelemetry's capture of
// BSShadowLight::ShadowmapDescriptor::lightTransform (descriptor offset 0x00),
// which requires SUN_CASCADE_CAPTURE_ON.
namespace SunCascadeBridge
{
    // Above the t0..tN range that custom-pass `input=` bindings occupy, and
    // one past LocalLightBridge's t36.
    inline constexpr UINT SRV_SLOT = 37;

    // Fallout's cascaded sun shader samples three cascades; a fourth is
    // captured when the engine submits one but is not published.
    inline constexpr UINT MAX_CASCADES = 3;

    // The shadow map array view, built by us.
    //
    // depthStencilTargets[6].srViewDepth exists but is a plain TEXTURE2D view
    // (confirmed in game: dimension=4, format=46 R24_UNORM_X8_TYPELESS), so it
    // reaches only one slice. The engine's own cascaded shader samples the
    // array with float3(uv, slice), so an array view must exist somewhere it
    // does not publish. Rather than hunt for it, we create our own
    // TEXTURE2DARRAY SRV over the same texture, which is fully under our
    // control and cannot be invalidated by engine bookkeeping we do not see.
    inline constexpr UINT SHADOW_ARRAY_SLOT = 38;

    void Shutdown();

    // Publishes the current frame's cascades and binds them for a custom pass.
    // The custom-pass state snapshot restores the previous binding.
    void BindCustomPassResource(
        REX::W32::ID3D11DeviceContext* context,
        bool pixelStage);
}
