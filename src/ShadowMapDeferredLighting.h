#pragma once

#include <PCH.h>

struct BSRenderPassLayout;

namespace ShadowMapDeferredLighting {

using Draw_t = void (*)(
    BSRenderPassLayout* pass,
    std::uintptr_t arg2,
    std::uintptr_t arg3,
    RE::BSGraphics::DynamicTriShapeDrawData* dynamicDrawData);

// Redirects a recognized BGS shadowed-local-light draw into isolated scratch
// MRTs, then composites that one light through the live shadow map. Returns
// true when the function consumed the original draw.
bool TryRender(
    BSRenderPassLayout* pass,
    std::uintptr_t arg2,
    std::uintptr_t arg3,
    RE::BSGraphics::DynamicTriShapeDrawData* dynamicDrawData,
    Draw_t draw) noexcept;

// Drops only the compiled filter shader. The next recognized draw recompiles
// it after Shader.ini/include hot reload.
void InvalidateShader() noexcept;

// Releases all D3D11 objects owned by the interior deferred-light pass.
void Shutdown() noexcept;

}  // namespace ShadowMapDeferredLighting
