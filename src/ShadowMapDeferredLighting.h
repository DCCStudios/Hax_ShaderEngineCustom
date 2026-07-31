#pragma once

#include <PCH.h>

struct BSRenderPassLayout;

namespace ShadowMapDeferredLighting {

using LiveDraw_t = void (*)(
    REX::W32::ID3D11DeviceContext* context,
    const void* payload) noexcept;

// Records the exact BSDFLight technique selected by the game. The technique is
// normalized with BSDFLightShaderMacros::GetPixelShaderID, matching the lookup
// key stored in Shaders011.fxp.
void ObservePass(BSRenderPassLayout* pass) noexcept;

// Called at the immediate-context Draw* boundary, after the command buffer has
// established the complete D3D11 contract. Redirects a recognized shadowed
// light into scratch MRTs and recomposites it through the live t5 shadow map.
// Returns true when the callback was issued for both replacement draws.
bool TryRenderLiveDraw(
    REX::W32::ID3D11DeviceContext* context,
    LiveDraw_t draw,
    const void* payload) noexcept;

// Drops only the compiled filter shader. The next recognized draw recompiles
// it after Shader.ini/include hot reload.
void InvalidateShader() noexcept;

// Releases all D3D11 objects owned by the map-space deferred-light pass.
void Shutdown() noexcept;

}  // namespace ShadowMapDeferredLighting
