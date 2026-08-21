#pragma once

#include <PCH.h>

namespace WaterTessellation
{
    // Marks the private VS/HS/DS pipeline stale. The actual COM release and
    // recompilation happen lazily on the render thread at the next matching draw.
    void RequestReload();

    // Replaces only the runtime-proven OG visualWater/O6 DrawIndexed contract.
    // Returns true after issuing the draw; false leaves the caller responsible
    // for the untouched engine draw.
    bool TryDrawIndexed(
        REX::W32::ID3D11DeviceContext* context,
        UINT indexCount,
        UINT startIndexLocation,
        INT baseVertexLocation);
}
