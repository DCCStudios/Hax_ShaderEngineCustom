#include <Global.h>
#include <LocalLightBridge.h>

namespace LocalLightBridge
{
namespace
{
    // Bethesda can reuse a non-tiled deferred-light command buffer for about
    // 120 frames. Keep two refresh intervals so a missed/reordered rebuild
    // cannot make a raster light disappear, then retire genuinely stale
    // copied records without ever dereferencing the old BSLight pointer.
    constexpr std::uint64_t kRasterKeepAliveFrames = 240;
    constexpr std::size_t kExpectedTiledLightCount = 128;
    constexpr float kViewSpaceKind = 0.0f;
    constexpr float kAbsoluteWorldKind = 1.0f;

    struct alignas(16) GpuLocalLight
    {
        DirectX::XMFLOAT4 positionAndRadius{};
        DirectX::XMFLOAT4 colorAndKind{};
        DirectX::XMFLOAT4 attenuation{};
    };

    static_assert(sizeof(GpuLocalLight) == 48);

    struct RasterLight
    {
        GpuLocalLight light{};
        std::uint64_t lastSeenFrame = 0;
    };

    struct PublicationCandidate
    {
        GpuLocalLight light{};
        std::uint64_t lastSeenFrame = 0;
    };

    std::mutex g_mutex;
    std::unordered_map<std::uintptr_t, RasterLight> g_rasterLights;
    std::vector<GpuLocalLight> g_tiledLights;
    std::vector<RasterLight> g_tiledFallbackLights;
    std::uint64_t g_frame = 0;
    bool g_uploadDirty = true;

    REX::W32::ID3D11Buffer* g_lightBuffer = nullptr;
    REX::W32::ID3D11ShaderResourceView* g_lightSRV = nullptr;

    bool Finite(float value) noexcept
    {
        return std::isfinite(value);
    }

    bool NearlyEqual(
        float left,
        float right,
        float absoluteTolerance,
        float relativeTolerance) noexcept
    {
        return std::abs(left - right) <=
            absoluteTolerance +
                relativeTolerance * (std::max)(std::abs(left), std::abs(right));
    }

    bool TiledAbsolutePosition(
        const GpuLocalLight& tiled,
        DirectX::XMFLOAT3& absolutePosition) noexcept
    {
        // Mirror HLSL mul(float4(viewPosition, 1), inverseView) exactly. The
        // renderer view is rotation-only; CameraStateData owns the absolute
        // translation applied after this transform.
        const auto& row0 = g_customBufferData.g_InvViewRow0;
        const auto& row1 = g_customBufferData.g_InvViewRow1;
        const auto& row2 = g_customBufferData.g_InvViewRow2;
        const auto& row3 = g_customBufferData.g_InvViewRow3;
        const auto& view = tiled.positionAndRadius;
        const float worldW =
            view.x * row0.w + view.y * row1.w +
            view.z * row2.w + row3.w;
        if (!Finite(worldW) || std::abs(worldW) <= 1.0e-6f) {
            return false;
        }

        const float inverseW = 1.0f / worldW;
        absolutePosition = {
            (view.x * row0.x + view.y * row1.x +
             view.z * row2.x + row3.x) * inverseW +
                g_customBufferData.g_CurrentCameraPositionAdjust.x,
            (view.x * row0.y + view.y * row1.y +
             view.z * row2.y + row3.y) * inverseW +
                g_customBufferData.g_CurrentCameraPositionAdjust.y,
            (view.x * row0.z + view.y * row1.z +
             view.z * row2.z + row3.z) * inverseW +
                g_customBufferData.g_CurrentCameraPositionAdjust.z
        };
        return Finite(absolutePosition.x) &&
               Finite(absolutePosition.y) &&
               Finite(absolutePosition.z);
    }

    bool MatchesAbsoluteLight(
        const GpuLocalLight& candidate,
        const DirectX::XMFLOAT3& candidateAbsolutePosition,
        const GpuLocalLight& retained) noexcept
    {
        const float radiusScale = (std::max)(
            1.0f,
            (std::max)(
                candidate.positionAndRadius.w,
                retained.positionAndRadius.w));
        const float positionTolerance = (std::max)(0.5f, radiusScale * 0.001f);
        const float deltaX =
            candidateAbsolutePosition.x - retained.positionAndRadius.x;
        const float deltaY =
            candidateAbsolutePosition.y - retained.positionAndRadius.y;
        const float deltaZ =
            candidateAbsolutePosition.z - retained.positionAndRadius.z;
        if (deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ >
            positionTolerance * positionTolerance) {
            return false;
        }
        if (!NearlyEqual(
                candidate.positionAndRadius.w,
                retained.positionAndRadius.w,
                0.5f,
                0.002f)) {
            return false;
        }

        // Position, radius, attenuation, and chroma identify a tiled light.
        // Bethesda can fade its intensity between submissions, so comparing
        // absolute RGB here would retain multiple copies of the same light.
        const auto& candidateColor = candidate.colorAndKind;
        const auto& retainedColor = retained.colorAndKind;
        const auto& candidateAttenuation = candidate.attenuation;
        const auto& retainedAttenuation = retained.attenuation;
        const float candidatePeak = (std::max)(
            candidateColor.x,
            (std::max)(candidateColor.y, candidateColor.z));
        const float retainedPeak = (std::max)(
            retainedColor.x,
            (std::max)(retainedColor.y, retainedColor.z));
        if (candidatePeak <= 1.0e-6f || retainedPeak <= 1.0e-6f) {
            return false;
        }
        const float candidateInversePeak = 1.0f / candidatePeak;
        const float retainedInversePeak = 1.0f / retainedPeak;
        return
            NearlyEqual(
                candidateColor.x * candidateInversePeak,
                retainedColor.x * retainedInversePeak,
                0.01f,
                0.02f) &&
            NearlyEqual(
                candidateColor.y * candidateInversePeak,
                retainedColor.y * retainedInversePeak,
                0.01f,
                0.02f) &&
            NearlyEqual(
                candidateColor.z * candidateInversePeak,
                retainedColor.z * retainedInversePeak,
                0.01f,
                0.02f) &&
            NearlyEqual(
                candidateAttenuation.x, retainedAttenuation.x, 0.001f, 0.01f) &&
            NearlyEqual(
                candidateAttenuation.y, retainedAttenuation.y, 0.001f, 0.01f) &&
            NearlyEqual(
                candidateAttenuation.z, retainedAttenuation.z, 0.001f, 0.01f);
    }

    bool CanonicalizeTiledLight(
        const GpuLocalLight& tiled,
        GpuLocalLight& canonical) noexcept
    {
        DirectX::XMFLOAT3 absolutePosition{};
        if (!TiledAbsolutePosition(tiled, absolutePosition)) {
            return false;
        }

        canonical = tiled;
        canonical.positionAndRadius.x = absolutePosition.x;
        canonical.positionAndRadius.y = absolutePosition.y;
        canonical.positionAndRadius.z = absolutePosition.z;
        canonical.colorAndKind.w = kAbsoluteWorldKind;
        return true;
    }

    bool IsValidLocalLight(const GpuLocalLight& light) noexcept
    {
        const auto& p = light.positionAndRadius;
        const auto& c = light.colorAndKind;
        const auto& a = light.attenuation;
        if (!Finite(p.x) || !Finite(p.y) || !Finite(p.z) || !Finite(p.w) ||
            !Finite(c.x) || !Finite(c.y) || !Finite(c.z) ||
            !Finite(a.x) || !Finite(a.y) || !Finite(a.z)) {
            return false;
        }

        // Radius zero is Bethesda's directional-fill reuse of local-light
        // shaders, not a point light. Reject invalid/extreme source data as
        // well so a corrupt engine record can never poison screen-space rays.
        if (p.w <= 1.0e-3f || p.w > 1.0e7f ||
            std::abs(p.x) > 1.0e9f ||
            std::abs(p.y) > 1.0e9f ||
            std::abs(p.z) > 1.0e9f) {
            return false;
        }
        if (c.x < 0.0f || c.y < 0.0f || c.z < 0.0f ||
            (c.x <= 1.0e-6f && c.y <= 1.0e-6f && c.z <= 1.0e-6f)) {
            return false;
        }
        // Fallout's stock shader feeds the finite exponent directly to pow().
        // Zero and unusually sharp profiles are valid; imposing a plugin-side
        // exponent range silently removes otherwise visible lights.
        return true;
    }

    float PublicationScore(const GpuLocalLight& light) noexcept
    {
        const auto& color = light.colorAndKind;
        const float peakColor = (std::max)(
            color.x,
            (std::max)(color.y, color.z));
        return peakColor * std::sqrt(
            (std::max)(1.0f, light.positionAndRadius.w));
    }

    bool PublicationOrder(
        const PublicationCandidate& left,
        const PublicationCandidate& right) noexcept
    {
        const bool leftCurrent = left.lastSeenFrame == g_frame;
        const bool rightCurrent = right.lastSeenFrame == g_frame;
        if (leftCurrent != rightCurrent) {
            return leftCurrent;
        }

        const float leftScore = PublicationScore(left.light);
        const float rightScore = PublicationScore(right.light);
        if (leftScore != rightScore) {
            return leftScore > rightScore;
        }
        if (left.lastSeenFrame != right.lastSeenFrame) {
            return left.lastSeenFrame > right.lastSeenFrame;
        }

        const auto& a = left.light.positionAndRadius;
        const auto& b = right.light.positionAndRadius;
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.w < b.w;
    }

    bool EnsureGpuResource(REX::W32::ID3D11Device* device)
    {
        if (!device) {
            return false;
        }

        if (!g_lightBuffer) {
            REX::W32::D3D11_BUFFER_DESC desc{};
            desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
            desc.byteWidth = sizeof(GpuLocalLight) * MAX_LIGHTS;
            desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            desc.miscFlags = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.structureByteStride = sizeof(GpuLocalLight);
            const HRESULT hr = device->CreateBuffer(&desc, nullptr, &g_lightBuffer);
            if (FAILED(hr)) {
                REX::WARN(
                    "LocalLightBridge: failed to create {}-light buffer (HRESULT 0x{:08X})",
                    MAX_LIGHTS,
                    static_cast<std::uint32_t>(hr));
                return false;
            }
        }

        if (!g_lightSRV) {
            REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.format = REX::W32::DXGI_FORMAT_UNKNOWN;
            desc.viewDimension = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
            desc.buffer.firstElement = 0;
            desc.buffer.numElements = MAX_LIGHTS;
            const HRESULT hr = device->CreateShaderResourceView(g_lightBuffer, &desc, &g_lightSRV);
            if (FAILED(hr)) {
                REX::WARN(
                    "LocalLightBridge: failed to create t{} SRV (HRESULT 0x{:08X})",
                    SRV_SLOT,
                    static_cast<std::uint32_t>(hr));
                return false;
            }
            REX::INFO(
                "LocalLightBridge: publishing up to {} Bethesda local lights at t{}",
                MAX_LIGHTS,
                SRV_SLOT);
        }
        return true;
    }
}

void Initialize()
{
    std::lock_guard lock(g_mutex);
    g_rasterLights.clear();
    g_tiledLights.clear();
    g_tiledLights.reserve(kExpectedTiledLightCount);
    g_tiledFallbackLights.clear();
    g_tiledFallbackLights.reserve(kExpectedTiledLightCount);
    g_frame = 0;
    g_uploadDirty = true;
}

void Shutdown()
{
    std::lock_guard lock(g_mutex);
    g_rasterLights.clear();
    g_tiledLights.clear();
    g_tiledFallbackLights.clear();
    if (g_lightSRV) {
        g_lightSRV->Release();
        g_lightSRV = nullptr;
    }
    if (g_lightBuffer) {
        g_lightBuffer->Release();
        g_lightBuffer = nullptr;
    }
    g_uploadDirty = true;
}

void OnRasterLight(
    const void* identity,
    const RE::NiPoint3* worldPosition,
    float radius,
    const RE::NiColor* linearColor,
    const RE::NiPoint3* attenuation)
{
    if (!SHADERENGINE_EFFECTS_ON || !identity || !worldPosition ||
        !linearColor || !attenuation) {
        return;
    }

    GpuLocalLight light{};
    light.positionAndRadius = {
        worldPosition->x,
        worldPosition->y,
        worldPosition->z,
        radius
    };
    light.colorAndKind = {
        linearColor->r,
        linearColor->g,
        linearColor->b,
        kAbsoluteWorldKind
    };
    light.attenuation = {
        attenuation->x,
        attenuation->y,
        attenuation->z,
        0.0f
    };
    if (!IsValidLocalLight(light)) {
        return;
    }

    std::lock_guard lock(g_mutex);
    g_rasterLights[reinterpret_cast<std::uintptr_t>(identity)] = {
        light,
        g_frame
    };
    g_uploadDirty = true;
}

void OnTiledLight(
    const RE::NiPoint3* viewPosition,
    float radius,
    const RE::NiColor* color,
    const RE::NiPoint3* attenuation)
{
    if (!SHADERENGINE_EFFECTS_ON || !viewPosition || !color || !attenuation) {
        return;
    }

    GpuLocalLight light{};
    light.positionAndRadius = {
        viewPosition->x,
        viewPosition->y,
        viewPosition->z,
        radius
    };
    light.colorAndKind = {
        color->r,
        color->g,
        color->b,
        kViewSpaceKind
    };
    light.attenuation = {
        attenuation->x,
        attenuation->y,
        attenuation->z,
        0.0f
    };
    if (!IsValidLocalLight(light)) {
        return;
    }

    std::lock_guard lock(g_mutex);
    const auto duplicate = std::find_if(
        g_tiledLights.begin(),
        g_tiledLights.end(),
        [&light](const GpuLocalLight& candidate) {
            return std::memcmp(&candidate, &light, sizeof(light)) == 0;
        });
    // MAX_LIGHTS is the final GPU publication limit, not a collection limit.
    // Bethesda's submission order is view-dependent, so truncating here made
    // otherwise stable lights appear and disappear as they crossed slot 48.
    if (duplicate == g_tiledLights.end()) {
        g_tiledLights.push_back(light);
    }
    g_uploadDirty = true;
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

    std::array<GpuLocalLight, MAX_LIGHTS> upload{};
    bool update = false;
    {
        std::lock_guard lock(g_mutex);
        if (g_uploadDirty) {
            // A light can be accepted by the tiled path before its cached
            // raster command ever executes SetupPointLightGeometry. Convert
            // current tiled records to absolute world space and retain them,
            // so a later tiled cutoff cannot remove the only bridge copy.
            std::vector<GpuLocalLight> transientTiled;
            transientTiled.reserve(g_tiledLights.size());
            for (const auto& tiled : g_tiledLights) {
                GpuLocalLight canonical{};
                if (!CanonicalizeTiledLight(tiled, canonical)) {
                    transientTiled.push_back(tiled);
                    continue;
                }

                const DirectX::XMFLOAT3 absolutePosition{
                    canonical.positionAndRadius.x,
                    canonical.positionAndRadius.y,
                    canonical.positionAndRadius.z
                };
                const auto retained = std::find_if(
                    g_tiledFallbackLights.begin(),
                    g_tiledFallbackLights.end(),
                    [&](const RasterLight& entry) {
                        return MatchesAbsoluteLight(
                            canonical,
                            absolutePosition,
                            entry.light);
                    });
                if (retained != g_tiledFallbackLights.end()) {
                    retained->light = canonical;
                    retained->lastSeenFrame = g_frame;
                } else {
                    g_tiledFallbackLights.push_back({ canonical, g_frame });
                }
            }

            // Build the complete deduplicated CPU candidate set before
            // applying the fixed GPU publication limit. Current-frame lights
            // win over retained fallbacks; within each set, prefer lights with
            // greater peak intensity and reach instead of Bethesda call order.
            std::vector<std::pair<std::uintptr_t, RasterLight>> raster;
            raster.reserve(g_rasterLights.size());
            for (const auto& entry : g_rasterLights) {
                raster.push_back(entry);
            }

            std::vector<PublicationCandidate> candidates;
            candidates.reserve(
                raster.size() + g_tiledFallbackLights.size() +
                transientTiled.size());
            for (const auto& entry : raster) {
                candidates.push_back({
                    entry.second.light,
                    entry.second.lastSeenFrame
                });
            }
            for (const auto& entry : g_tiledFallbackLights) {
                const auto& position = entry.light.positionAndRadius;
                const DirectX::XMFLOAT3 absolutePosition{
                    position.x,
                    position.y,
                    position.z
                };
                const bool duplicate = std::any_of(
                    raster.begin(),
                    raster.end(),
                    [&](const auto& rasterEntry) {
                        return MatchesAbsoluteLight(
                            entry.light,
                            absolutePosition,
                            rasterEntry.second.light);
                    });
                if (!duplicate) {
                    candidates.push_back({
                        entry.light,
                        entry.lastSeenFrame
                    });
                }
            }
            // Matrix data is unavailable only during startup. Preserve those
            // current-frame records without attempting persistence.
            for (const auto& entry : transientTiled) {
                candidates.push_back({ entry, g_frame });
            }

            std::sort(
                candidates.begin(),
                candidates.end(),
                PublicationOrder);
            const std::size_t count = (std::min)(
                candidates.size(),
                upload.size());
            for (std::size_t index = 0; index < count; ++index) {
                upload[index] = candidates[index].light;
            }
            g_uploadDirty = false;
            update = true;
        }
    }

    if (update) {
        // The pass snapshot will restore both stages. Unbind before updating
        // so the debug layer never sees a read/write hazard on this buffer.
        REX::W32::ID3D11ShaderResourceView* nullSRV = nullptr;
        context->PSSetShaderResources(SRV_SLOT, 1, &nullSRV);
        context->CSSetShaderResources(SRV_SLOT, 1, &nullSRV);
        context->UpdateSubresource(g_lightBuffer, 0, nullptr, upload.data(), 0, 0);
    }

    if (pixelStage) {
        context->PSSetShaderResources(SRV_SLOT, 1, &g_lightSRV);
    } else {
        context->CSSetShaderResources(SRV_SLOT, 1, &g_lightSRV);
    }
}

void OnFramePresent()
{
    std::lock_guard lock(g_mutex);
    ++g_frame;
    g_tiledLights.clear();
    for (auto it = g_rasterLights.begin(); it != g_rasterLights.end();) {
        if (g_frame - it->second.lastSeenFrame > kRasterKeepAliveFrames) {
            it = g_rasterLights.erase(it);
        } else {
            ++it;
        }
    }
    g_tiledFallbackLights.erase(
        std::remove_if(
            g_tiledFallbackLights.begin(),
            g_tiledFallbackLights.end(),
            [](const RasterLight& entry) {
                return g_frame - entry.lastSeenFrame >
                    kRasterKeepAliveFrames;
            }),
        g_tiledFallbackLights.end());
    // Force a zero-filled upload next frame even when no local light draws.
    g_uploadDirty = true;
}
}
