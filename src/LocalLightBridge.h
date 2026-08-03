#pragma once

#include <PCH.h>

// Bridges Bethesda's raster and tiled deferred point lights into one
// structured SRV for screen-space custom passes. Raster lights are retained
// across the engine's cached-command-buffer refresh interval; tiled lights are
// rebuilt every frame by BSDFTiledLighting::AddLight.
namespace LocalLightBridge
{
    inline constexpr UINT SRV_SLOT = 36;
    inline constexpr UINT MAX_LIGHTS = 48;

    void Initialize();
    void Shutdown();

    // Called from BSDFLightShader::SetupPointLightGeometry with the source
    // BSLight's stable identity and authoritative absolute-world fields.
    void OnRasterLight(
        const void* identity,
        const RE::NiPoint3* worldPosition,
        float radius,
        const RE::NiColor* linearColor,
        const RE::NiPoint3* attenuation);

    // Called by the existing BSDFTiledLighting::AddLight redirect. The caller
    // subtracts camera translation and applies the camera's inverse rotation.
    void OnTiledLight(
        const RE::NiPoint3* viewPosition,
        float radius,
        const RE::NiColor* color,
        const RE::NiPoint3* attenuation);

    // Publishes the accumulated frame data and binds it to t36 for a custom
    // pass. The custom-pass state snapshot restores the previous t36 binding.
    void BindCustomPassResource(
        REX::W32::ID3D11DeviceContext* context,
        bool pixelStage);

    // Clears the completed frame's tiled list and ages retained raster lights.
    void OnFramePresent();
}
