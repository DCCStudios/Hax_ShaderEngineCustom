#include <PCH.h>
#include "ContactShadowBridge.h"

#include "Global.h"
#include "Plugin.h"
#include "RenderTargets.h"
#include "ShadowTelemetry.h"

#include <atomic>
#include <chrono>
#include <cmath>

namespace ContactShadowBridge
{
namespace
{
    // Mirrors SEContactDispatch in contactShadowRaymarch.hlsl (48 bytes).
    struct GpuContactDispatch
    {
        float lightCoordinate[4] = {};
        int   waveOffset[2]      = {};
        int   waveCount[2]       = {};  // WaveCount[1], WaveCount[2]
        int   flatStart          = 0;
        int   flatCount          = 0;
        int   gridWidth          = 0;
        int   liveTotalGroups    = 0;
        // Render extent in pixels. Under DLSS the depth allocation is
        // display-sized but only its top-left subrect is rendered, so the
        // shader has to bound its border test against this rather than against
        // the texture dimensions - otherwise every ray that leaves the render
        // area reads stale pixels from the unrendered margin instead of
        // resolving to far depth.
        int   renderExtent[2]    = {};
        int   pad[2]             = {};
    };
    static_assert(sizeof(GpuContactDispatch) == 64,
                  "SEContactDispatch layout must match HLSL");

    REX::W32::ID3D11Buffer*             g_buffer = nullptr;
    REX::W32::ID3D11ShaderResourceView* g_srv    = nullptr;

    // ---- Bend Studio's BuildDispatchList -----------------------------------
    //
    // Ported verbatim in structure from bend_sss_cpu.h (Apache-2.0). The only
    // changes are naming and the removal of the expanded-Z-range option, which
    // Fallout 4 does not need. This is pure integer/float math with no engine
    // dependencies, so it is reproduced rather than reinterpreted - the wave
    // partitioning is subtle and there is no benefit to rewriting it.

    struct DispatchData
    {
        int waveCount[3]  = {};
        int waveOffset[2] = {};
    };

    struct DispatchList
    {
        float        lightCoordinate[4] = {};
        DispatchData dispatch[MAX_DISPATCHES]{};
        int          dispatchCount = 0;
    };

    constexpr int BendMin(int a, int b) { return a > b ? b : a; }
    constexpr int BendMax(int a, int b) { return a > b ? a : b; }

    DispatchList BuildDispatchList(
        const float lightProjection[4],
        const int   viewportSize[2],
        const int   minRenderBounds[2],
        const int   maxRenderBounds[2],
        int         waveSize)
    {
        DispatchList result{};

        // Float division in the shader loses precision when the light is very
        // far off screen, so clamp the w used for the XY coordinate.
        float xyLightW = lightProjection[3];
        const float fpLimit = 0.000002f * static_cast<float>(waveSize);
        if (xyLightW >= 0.0f && xyLightW < fpLimit) {
            xyLightW = fpLimit;
        } else if (xyLightW < 0.0f && xyLightW > -fpLimit) {
            xyLightW = -fpLimit;
        }

        result.lightCoordinate[0] =
            ((lightProjection[0] / xyLightW) * +0.5f + 0.5f) *
            static_cast<float>(viewportSize[0]);
        result.lightCoordinate[1] =
            ((lightProjection[1] / xyLightW) * -0.5f + 0.5f) *
            static_cast<float>(viewportSize[1]);
        result.lightCoordinate[2] = lightProjection[3] == 0.0f
            ? 0.0f
            : (lightProjection[2] / lightProjection[3]);
        result.lightCoordinate[3] = lightProjection[3] > 0.0f ? 1.0f : -1.0f;

        const int lightXY[2] = {
            static_cast<int>(result.lightCoordinate[0] + 0.5f),
            static_cast<int>(result.lightCoordinate[1] + 0.5f),
        };

        // Inclusive bounds, relative to the light.
        const int biasedBounds[4] = {
            minRenderBounds[0] - lightXY[0],
            -(maxRenderBounds[1] - lightXY[1]),
            maxRenderBounds[0] - lightXY[0],
            -(minRenderBounds[1] - lightXY[1]),
        };

        // Four quadrants around the light centre, each a rectangle with one
        // corner on the light. A non-square rectangle splits on its larger axis.
        for (int q = 0; q < 4; ++q) {
            const bool vertical = (q == 0 || q == 3);

            const int bounds[4] = {
                BendMax(0, ((q & 1) ? biasedBounds[0] : -biasedBounds[2])) / waveSize,
                BendMax(0, ((q & 2) ? biasedBounds[1] : -biasedBounds[3])) / waveSize,
                BendMax(0, (((q & 1) ? biasedBounds[2] : -biasedBounds[0]) +
                            waveSize * (vertical ? 1 : 2) - 1)) / waveSize,
                BendMax(0, (((q & 2) ? biasedBounds[3] : -biasedBounds[1]) +
                            waveSize * (vertical ? 2 : 1) - 1)) / waveSize,
            };

            if ((bounds[2] - bounds[0]) <= 0 || (bounds[3] - bounds[1]) <= 0) {
                continue;
            }
            if (result.dispatchCount >= static_cast<int>(MAX_DISPATCHES)) {
                break;
            }

            const int biasX = (q == 2 || q == 3) ? 1 : 0;
            const int biasY = (q == 1 || q == 3) ? 1 : 0;

            DispatchData& disp = result.dispatch[result.dispatchCount++];
            disp.waveCount[0] = waveSize;
            disp.waveCount[1] = bounds[2] - bounds[0];
            disp.waveCount[2] = bounds[3] - bounds[1];
            disp.waveOffset[0] = ((q & 1) ? bounds[0] : -bounds[2]) + biasX;
            disp.waveOffset[1] = ((q & 2) ? -bounds[3] : bounds[1]) + biasY;

            // Where the diagonal light ray crosses the edge of the bounds.
            int axisDelta = +biasedBounds[0] - biasedBounds[1];
            if (q == 1) axisDelta = +biasedBounds[2] + biasedBounds[1];
            if (q == 2) axisDelta = -biasedBounds[0] - biasedBounds[3];
            if (q == 3) axisDelta = -biasedBounds[2] + biasedBounds[3];
            axisDelta = (axisDelta + waveSize - 1) / waveSize;

            if (axisDelta <= 0) {
                continue;
            }
            if (result.dispatchCount >= static_cast<int>(MAX_DISPATCHES)) {
                continue;
            }

            DispatchData& disp2 = result.dispatch[result.dispatchCount++];
            disp2 = disp;

            if (q == 0) {
                disp2.waveCount[2] = BendMin(disp.waveCount[2], axisDelta);
                disp.waveCount[2] -= disp2.waveCount[2];
                disp2.waveOffset[1] = disp.waveOffset[1] + disp.waveCount[2];
                disp2.waveOffset[0]--;
                disp2.waveCount[1]++;
            } else if (q == 1) {
                disp2.waveCount[1] = BendMin(disp.waveCount[1], axisDelta);
                disp.waveCount[1] -= disp2.waveCount[1];
                disp2.waveOffset[0] = disp.waveOffset[0] + disp.waveCount[1];
                disp2.waveCount[2]++;
            } else if (q == 2) {
                disp2.waveCount[1] = BendMin(disp.waveCount[1], axisDelta);
                disp.waveCount[1] -= disp2.waveCount[1];
                disp.waveOffset[0] += disp2.waveCount[1];
                disp2.waveCount[2]++;
                disp2.waveOffset[1]--;
            } else {
                disp2.waveCount[2] = BendMin(disp.waveCount[2], axisDelta);
                disp.waveCount[2] -= disp2.waveCount[2];
                disp.waveOffset[1] += disp2.waveCount[2];
                disp2.waveCount[1]++;
            }

            // Drop either volume if the split emptied it. Order matters: disp2
            // is the newer entry, so it must be collapsed first.
            if (disp2.waveCount[1] <= 0 || disp2.waveCount[2] <= 0) {
                disp2 = result.dispatch[--result.dispatchCount];
            }
            if (disp.waveCount[1] <= 0 || disp.waveCount[2] <= 0) {
                disp = result.dispatch[--result.dispatchCount];
            }
        }

        // The shader expects wave offsets in pixels.
        for (int i = 0; i < result.dispatchCount; ++i) {
            result.dispatch[i].waveOffset[0] *= waveSize;
            result.dispatch[i].waveOffset[1] *= waveSize;
        }

        return result;
    }

    // ---- ShaderEngine plumbing ---------------------------------------------

    // Reproduces CustomPass's `screenceil/N` resolution, which measures against
    // renderTargets[kMain], NOT the depth allocation or the DLSS render extent.
    bool BackbufferExtent(std::uint32_t& outWidth, std::uint32_t& outHeight)
    {
        if (!g_rendererData) {
            return false;
        }
        auto* texture =
            g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture;
        if (!texture) {
            return false;
        }
        REX::W32::D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (!desc.width || !desc.height) {
            return false;
        }
        outWidth = desc.width;
        outHeight = desc.height;
        return true;
    }

    constexpr std::uint32_t CeilDiv(std::uint32_t v, std::uint32_t d)
    {
        return d ? ((v + d - 1) / d) : 1u;
    }

    bool EnsureGpuResource(REX::W32::ID3D11Device* device)
    {
        if (!device) {
            return false;
        }

        if (!g_buffer) {
            REX::W32::D3D11_BUFFER_DESC desc{};
            desc.usage               = REX::W32::D3D11_USAGE_DEFAULT;
            desc.byteWidth           = sizeof(GpuContactDispatch) * MAX_DISPATCHES;
            desc.bindFlags           = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            desc.miscFlags           = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.structureByteStride = sizeof(GpuContactDispatch);
            const HRESULT hr = device->CreateBuffer(&desc, nullptr, &g_buffer);
            if (FAILED(hr)) {
                REX::WARN(
                    "ContactShadowBridge: failed to create dispatch buffer "
                    "(HRESULT 0x{:08X})",
                    static_cast<std::uint32_t>(hr));
                return false;
            }
        }

        if (!g_srv) {
            REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.format              = REX::W32::DXGI_FORMAT_UNKNOWN;
            desc.viewDimension       = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
            desc.buffer.firstElement = 0;
            desc.buffer.numElements  = MAX_DISPATCHES;
            const HRESULT hr =
                device->CreateShaderResourceView(g_buffer, &desc, &g_srv);
            if (FAILED(hr)) {
                REX::WARN(
                    "ContactShadowBridge: failed to create t{} SRV "
                    "(HRESULT 0x{:08X})",
                    SRV_SLOT,
                    static_cast<std::uint32_t>(hr));
                return false;
            }
        }

        return g_buffer && g_srv;
    }
}

void Shutdown()
{
    if (g_srv) {
        g_srv->Release();
        g_srv = nullptr;
    }
    if (g_buffer) {
        g_buffer->Release();
        g_buffer = nullptr;
    }
}

void BindCustomPassResource(
    REX::W32::ID3D11DeviceContext* context,
    bool pixelStage)
{
    if (!context) {
        return;
    }

    REX::W32::ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (!device) {
        return;
    }
    const bool ready = EnsureGpuResource(device);
    device->Release();
    if (!ready) {
        return;
    }

    GpuContactDispatch upload[MAX_DISPATCHES]{};

    // A failed precondition publishes zero live groups rather than a guessed
    // list. The shader reads liveTotalGroups == 0 as "no contact shadow this
    // frame" and leaves the mask untouched, which reads as fully lit - never as
    // fully shadowed.
    std::uint32_t backbufferW = 0;
    std::uint32_t backbufferH = 0;
    const auto& gfx = g_customBufferData;

    // The wavefront march works entirely in pixels, so the light's pixel
    // coordinate and the depth reads must agree on which pixel space that is.
    // The grid width follows CustomPass, which measures renderTargets[kMain],
    // but the light coordinate must be expressed in RENDER pixels: under DLSS
    // the depth allocation is display-sized and only its top-left subrect is
    // rendered. Deriving the light position from the allocation instead would
    // shift it by the upscale ratio and skew every ray in the frame.
    constexpr float kMinimumRenderExtent = 16.0f;
    const bool renderExtentValid =
        std::isfinite(gfx.g_RenderInfo.x) && std::isfinite(gfx.g_RenderInfo.y) &&
        gfx.g_RenderInfo.x >= kMinimumRenderExtent &&
        gfx.g_RenderInfo.y >= kMinimumRenderExtent;

    // The sun direction comes from the captured shadow cascade, NOT from
    // g_SunDirX/Y/Z. That triple is HachiToon's *stylized* dominant light
    // direction, and in game it logs as a constant (0,-1,0) that never moves
    // with time of day or weather - feeding it here marched every shadow ray
    // in a fixed horizontal direction, which darkened everything uniformly
    // and put a short smear halo around every silhouette. The cascade
    // transform is the engine's actual sun: world -> shadow UV, row-major,
    // so its third COLUMN is the axis that maps world position to light
    // depth - the sun's forward vector. Same capture skylighting's sun
    // visibility already uses, confirmed in game via debug mode 18.
    ShadowTelemetry::DirectionalCascade cascade{};
    float sunDir[3] = { 0.0f, 0.0f, 0.0f };
    bool sunDirValid = false;
    if (ShadowTelemetry::SunCascadesLookValid() &&
        ShadowTelemetry::GetDirectionalCascades(&cascade, 1) > 0 &&
        cascade.valid) {
        float d[3] = {
            cascade.transform[2],
            cascade.transform[6],
            cascade.transform[10],
        };
        const float lengthSq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (std::isfinite(lengthSq) && lengthSq > 1e-12f) {
            const float inverseLength = 1.0f / std::sqrt(lengthSq);
            d[0] *= inverseLength;
            d[1] *= inverseLength;
            d[2] *= inverseLength;
            // Bend wants the homogeneous point at infinity toward the LIGHT -
            // the sun's position in the sky. The cascade depth axis's sign
            // convention is unverified (the projection carries a negative Z
            // scale), so disambiguate physically: a shadow-casting sun is
            // above the horizon, so the toward-light vector points up.
            if (d[2] < 0.0f) {
                d[0] = -d[0];
                d[1] = -d[1];
                d[2] = -d[2];
            }
            sunDir[0] = d[0];
            sunDir[1] = d[1];
            sunDir[2] = d[2];
            sunDirValid = true;
        }
    }

    // No valid cascade (interior, night without a shadow-casting light, or
    // capture disabled) publishes zero work; the composite reads that as
    // identity, so the correct "no sun contact shadows here" falls out.
    const bool inputsValid =
        BackbufferExtent(backbufferW, backbufferH) && sunDirValid;

    int totalGroups = 0;
    const std::uint32_t gridWidth = CeilDiv(backbufferW, GRID_DIV_X);

    const int renderExtent[2] = {
        static_cast<int>(renderExtentValid
            ? std::lround(gfx.g_RenderInfo.x)
            : static_cast<long>(backbufferW)),
        static_cast<int>(renderExtentValid
            ? std::lround(gfx.g_RenderInfo.y)
            : static_cast<long>(backbufferH)),
    };

    if (inputsValid) {
        // sunDir is already unit length from the cascade extraction.
        const float* direction = sunDir;

        // inLightProjection = float4(direction, 0) * ViewProjection.
        // The shader-side convention is mul(float4(pos, 1), M) with M built
        // from g_ViewProjRow0..3 as ROWS, i.e. a row-vector multiply, so the
        // CPU form is the same weighted sum of those rows. With w = 0 the
        // translation row drops out, which is also why it does not matter that
        // the engine's transform expects camera-relative positions.
        const auto& r0 = gfx.g_ViewProjRow0;
        const auto& r1 = gfx.g_ViewProjRow1;
        const auto& r2 = gfx.g_ViewProjRow2;
        const float lightProjection[4] = {
            direction[0] * r0.x + direction[1] * r1.x + direction[2] * r2.x,
            direction[0] * r0.y + direction[1] * r1.y + direction[2] * r2.y,
            direction[0] * r0.z + direction[1] * r1.z + direction[2] * r2.z,
            direction[0] * r0.w + direction[1] * r1.w + direction[2] * r2.w,
        };

        const int viewportSize[2] = { renderExtent[0], renderExtent[1] };
        const int minBounds[2] = { 0, 0 };
        const int maxBounds[2] = { viewportSize[0], viewportSize[1] };

        const DispatchList list = BuildDispatchList(
            lightProjection, viewportSize, minBounds, maxBounds, WAVE_SIZE);

        for (int i = 0; i < list.dispatchCount; ++i) {
            const DispatchData& d = list.dispatch[i];
            const int groups =
                d.waveCount[0] * d.waveCount[1] * d.waveCount[2];
            if (groups <= 0) {
                continue;
            }
            auto& entry = upload[i];
            std::memcpy(entry.lightCoordinate, list.lightCoordinate,
                        sizeof(entry.lightCoordinate));
            entry.waveOffset[0] = d.waveOffset[0];
            entry.waveOffset[1] = d.waveOffset[1];
            entry.waveCount[0]  = d.waveCount[1];
            entry.waveCount[1]  = d.waveCount[2];
            entry.flatStart     = totalGroups;
            entry.flatCount     = groups;
            totalGroups += groups;
        }

        // The dispatched grid is an upper bound; if the list somehow exceeds it
        // the tail would be silently dropped, so refuse the frame instead.
        const std::uint32_t gridHeight = CeilDiv(backbufferH, GRID_DIV_Y);
        const std::uint64_t capacity =
            static_cast<std::uint64_t>(gridWidth) * gridHeight;
        if (static_cast<std::uint64_t>(totalGroups) > capacity) {
            REX::WARN(
                "ContactShadowBridge: dispatch list needs {} groups but the "
                "grid provides {} ({}x{}); disabling this frame",
                totalGroups, capacity, gridWidth, gridHeight);
            totalGroups = 0;
        }
    }

    // Every entry carries the header fields, because the shader reads entry 0
    // before it knows which entry owns its group.
    for (auto& entry : upload) {
        entry.gridWidth       = static_cast<int>(gridWidth);
        entry.liveTotalGroups = totalGroups;
        entry.renderExtent[0] = renderExtent[0];
        entry.renderExtent[1] = renderExtent[1];
    }
    if (totalGroups == 0) {
        for (auto& entry : upload) {
            entry.flatCount = 0;
        }
    }

    context->UpdateSubresource(g_buffer, 0, nullptr, upload, 0, 0);

    // Throttled state dump. This path used to fail silently: any unmet
    // precondition published zero groups, the compute pass returned without
    // writing, and the mask kept its zero-initialised contents - which the
    // composite reads as "fully occluded everywhere", producing a flat
    // strength-scaled darkening with no light direction in it at all. That is
    // indistinguishable at a glance from a badly tuned march, and cost several
    // rounds of tuning a shader that was never running.
    {
        static std::atomic<std::uint64_t> lastLogMs{0};
        const auto nowMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const auto previous = lastLogMs.load(std::memory_order_relaxed);
        if (nowMs - previous > 5000u) {
            lastLogMs.store(nowMs, std::memory_order_relaxed);
            if (totalGroups > 0) {
                REX::INFO(
                    "ContactShadowBridge: light px=({:.1f},{:.1f}) z={:.4f} "
                    "w={:.1f} | dispatches with work, {} groups | render {}x{} "
                    "grid {} | sunDir=({:.3f},{:.3f},{:.3f}) valid={:.1f}",
                    upload[0].lightCoordinate[0], upload[0].lightCoordinate[1],
                    upload[0].lightCoordinate[2], upload[0].lightCoordinate[3],
                    totalGroups, renderExtent[0], renderExtent[1], gridWidth,
                    sunDir[0], sunDir[1], sunDir[2],
                    sunDirValid ? 1.0f : 0.0f);
            } else {
                REX::WARN(
                    "ContactShadowBridge: publishing NO work - contact shadows "
                    "will not be written this frame. backbuffer={}x{} "
                    "cascadeSunDirValid={} renderExtentValid={}",
                    backbufferW, backbufferH, sunDirValid, renderExtentValid);
            }
        }
    }

    if (pixelStage) {
        context->PSSetShaderResources(SRV_SLOT, 1, &g_srv);
    } else {
        context->CSSetShaderResources(SRV_SLOT, 1, &g_srv);
    }
}
}
