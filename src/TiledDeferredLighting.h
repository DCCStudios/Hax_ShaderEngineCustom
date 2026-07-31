#pragma once

#include <PCH.h>

struct BSRenderPassLayout;

namespace TiledDeferredLighting {

using LiveDraw_t = void (*)(
    REX::W32::ID3D11DeviceContext* context,
    const void* payload) noexcept;

// Captures the same local-light arguments forwarded to
// BSDFTiledLighting::AddLight. Calls are expected on the render thread.
void RecordLight(
    std::uint32_t id,
    const RE::NiPoint3* position,
    float radius,
    const RE::NiColor* color,
    const RE::NiPoint3* direction,
    bool flagA,
    bool flagB,
    bool flagC,
    bool flagD) noexcept;

// Delimits DrawWorld::DeferredLightsImpl. AddLight runs before this scope;
// EndDeferredLights publishes the captured list to the following deferred
// composite draw.
void BeginDeferredLights() noexcept;
void EndDeferredLights() noexcept;

// Records the exact DFComposite tiled-light permutation selected by the game.
void ObserveComposite(BSRenderPassLayout* pass) noexcept;

// Processes the native tiled diffuse/specular accumulators at the live D3D11
// Draw* boundary. Returns true when the callback was issued for both the
// processor and native composite draws.
bool TryRenderLiveComposite(
    REX::W32::ID3D11DeviceContext* context,
    LiveDraw_t draw,
    const void* payload) noexcept;

// Drops the compiled processor so Shader.ini/include hot reload can rebuild
// it with the current modular settings.
void InvalidateShader() noexcept;

// Releases all D3D11 resources owned by the tiled-light processor.
void Shutdown() noexcept;

}  // namespace TiledDeferredLighting
