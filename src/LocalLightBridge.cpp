#include <Global.h>
#include <LightSorter.h>
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
    constexpr std::size_t kExpectedTiledLightCount = MAX_LIGHTS;
    constexpr float kViewSpaceKind = 0.0f;
    constexpr float kAbsoluteWorldKind = 1.0f;
    // attenuation.w is unused by Bethesda's three-component attenuation
    // equation. Reuse it as bridge metadata for Voxel GI visibility.
    constexpr float kVoxelShadowEligible = 1.0f;

    struct alignas(16) GpuLocalLight
    {
        DirectX::XMFLOAT4 positionAndRadius{};
        DirectX::XMFLOAT4 colorAndKind{};
        DirectX::XMFLOAT4 attenuation{};
    };

    static_assert(sizeof(GpuLocalLight) == 48);

    // The radius written to the GPU record can be expanded by directLightMul
    // so Bethesda's tiled and volume-light paths cover the same pixels. The
    // engine shadow array retains the native BSLight radius. Keep both values
    // on the CPU without changing the 48-byte shader ABI.
    struct ObservedLight
    {
        GpuLocalLight light{};
        float nativeRadius = 0.0f;
    };

    struct RasterLight
    {
        ObservedLight observation{};
        std::uint64_t lastSeenFrame = 0;
    };

    struct PublicationCandidate
    {
        ObservedLight observation{};
    };

    struct CorrelationStats
    {
        std::size_t eligibleCandidates = 0;
        std::size_t positionMatches = 0;
        std::size_t radiusMatches = 0;
        std::size_t uniqueMatches = 0;
        std::size_t ambiguousMatches = 0;
    };

    std::mutex g_mutex;
    std::unordered_map<std::uintptr_t, RasterLight> g_rasterLights;
    std::vector<ObservedLight> g_tiledLights;
    std::vector<RasterLight> g_tiledFallbackLights;
    std::uint64_t g_frame = 0;
    std::uint64_t g_lastStatsFrame = 0;
    bool g_uploadDirty = true;
    bool g_overflowLogged = false;
    bool g_statsLogged = false;

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

    float MatchRadius(const ObservedLight& observed) noexcept
    {
        if (Finite(observed.nativeRadius) && observed.nativeRadius > 1.0e-3f) {
            return observed.nativeRadius;
        }
        return observed.light.positionAndRadius.w;
    }

    bool PositionMatches(
        const ObservedLight& candidate,
        const DirectX::XMFLOAT3& candidateAbsolutePosition,
        const ObservedLight& retained) noexcept
    {
        const float radiusScale = (std::max)(
            1.0f,
            (std::max)(
                MatchRadius(candidate),
                MatchRadius(retained)));
        const float positionTolerance = (std::max)(1.0f, radiusScale * 0.0025f);
        const float deltaX =
            candidateAbsolutePosition.x - retained.light.positionAndRadius.x;
        const float deltaY =
            candidateAbsolutePosition.y - retained.light.positionAndRadius.y;
        const float deltaZ =
            candidateAbsolutePosition.z - retained.light.positionAndRadius.z;
        return deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ <=
            positionTolerance * positionTolerance;
    }

    bool RadiusMatches(
        const ObservedLight& candidate,
        const ObservedLight& retained) noexcept
    {
        return NearlyEqual(
            MatchRadius(candidate),
            MatchRadius(retained),
            1.0f,
            0.005f);
    }

    float AppearanceDifference(
        const GpuLocalLight& candidate,
        const GpuLocalLight& retained) noexcept
    {
        const auto& candidateColor = candidate.colorAndKind;
        const auto& retainedColor = retained.colorAndKind;
        const float candidatePeak = (std::max)(
            candidateColor.x,
            (std::max)(candidateColor.y, candidateColor.z));
        const float retainedPeak = (std::max)(
            retainedColor.x,
            (std::max)(retainedColor.y, retainedColor.z));
        if (candidatePeak <= 1.0e-6f || retainedPeak <= 1.0e-6f) {
            return (std::numeric_limits<float>::max)();
        }

        const float candidateInversePeak = 1.0f / candidatePeak;
        const float retainedInversePeak = 1.0f / retainedPeak;
        const float colorDeltaX =
            candidateColor.x * candidateInversePeak -
            retainedColor.x * retainedInversePeak;
        const float colorDeltaY =
            candidateColor.y * candidateInversePeak -
            retainedColor.y * retainedInversePeak;
        const float colorDeltaZ =
            candidateColor.z * candidateInversePeak -
            retainedColor.z * retainedInversePeak;

        const auto& candidateAttenuation = candidate.attenuation;
        const auto& retainedAttenuation = retained.attenuation;
        const auto normalizedDelta = [](float left, float right) noexcept {
            const float scale = (std::max)(
                0.01f,
                (std::max)(std::abs(left), std::abs(right)));
            return (left - right) / scale;
        };
        const float attenuationDeltaX = normalizedDelta(
            candidateAttenuation.x, retainedAttenuation.x);
        const float attenuationDeltaY = normalizedDelta(
            candidateAttenuation.y, retainedAttenuation.y);
        const float attenuationDeltaZ = normalizedDelta(
            candidateAttenuation.z, retainedAttenuation.z);
        return
            colorDeltaX * colorDeltaX +
            colorDeltaY * colorDeltaY +
            colorDeltaZ * colorDeltaZ +
            0.25f * (
                attenuationDeltaX * attenuationDeltaX +
                attenuationDeltaY * attenuationDeltaY +
                attenuationDeltaZ * attenuationDeltaZ);
    }

    bool MatchesAbsoluteLight(
        const ObservedLight& candidate,
        const DirectX::XMFLOAT3& candidateAbsolutePosition,
        const ObservedLight& retained) noexcept
    {
        if (!PositionMatches(candidate, candidateAbsolutePosition, retained) ||
            !RadiusMatches(candidate, retained)) {
            return false;
        }

        // Position, radius, attenuation, and chroma identify a tiled light.
        // Bethesda can fade its intensity between submissions, so comparing
        // absolute RGB here would retain multiple copies of the same light.
        return AppearanceDifference(candidate.light, retained.light) <= 0.02f;
    }

    bool CorrelateEngineShadow(
        const ObservedLight& candidate,
        const DirectX::XMFLOAT3& candidateAbsolutePosition,
        const std::vector<ObservedLight>& engineShadowLights,
        CorrelationStats& stats) noexcept
    {
        ++stats.eligibleCandidates;

        std::array<std::size_t, MAX_LIGHTS> positionMatches{};
        std::size_t positionMatchCount = 0;
        for (std::size_t index = 0; index < engineShadowLights.size(); ++index) {
            if (PositionMatches(
                    candidate,
                    candidateAbsolutePosition,
                    engineShadowLights[index])) {
                positionMatches[positionMatchCount++] = index;
            }
        }
        if (positionMatchCount == 0) {
            return false;
        }
        ++stats.positionMatches;

        std::array<std::size_t, MAX_LIGHTS> radiusMatches{};
        std::size_t radiusMatchCount = 0;
        for (std::size_t match = 0; match < positionMatchCount; ++match) {
            const std::size_t index = positionMatches[match];
            if (RadiusMatches(candidate, engineShadowLights[index])) {
                radiusMatches[radiusMatchCount++] = index;
            }
        }
        if (radiusMatchCount == 0) {
            return false;
        }
        ++stats.radiusMatches;
        if (radiusMatchCount == 1) {
            ++stats.uniqueMatches;
            return true;
        }

        float bestDifference = (std::numeric_limits<float>::max)();
        float secondDifference = (std::numeric_limits<float>::max)();
        for (std::size_t match = 0; match < radiusMatchCount; ++match) {
            const float difference = AppearanceDifference(
                candidate.light,
                engineShadowLights[radiusMatches[match]].light);
            if (difference < bestDifference) {
                secondDifference = bestDifference;
                bestDifference = difference;
            } else if (difference < secondDifference) {
                secondDifference = difference;
            }
        }

        const float requiredSeparation = (std::max)(
            0.05f,
            bestDifference * 0.2f);
        if (Finite(bestDifference) &&
            secondDifference - bestDifference > requiredSeparation) {
            ++stats.uniqueMatches;
            return true;
        }

        // Multiple engine lights can intentionally share a transform. Leave
        // an ambiguous candidate eligible rather than suppressing a different
        // unshadowed light and record the case for the diagnostics overlay/log.
        ++stats.ambiguousMatches;
        return false;
    }

    bool CanonicalizeTiledLight(
        const ObservedLight& tiled,
        ObservedLight& canonical) noexcept
    {
        DirectX::XMFLOAT3 absolutePosition{};
        if (!TiledAbsolutePosition(tiled.light, absolutePosition)) {
            return false;
        }

        canonical = tiled;
        canonical.light.positionAndRadius.x = absolutePosition.x;
        canonical.light.positionAndRadius.y = absolutePosition.y;
        canonical.light.positionAndRadius.z = absolutePosition.z;
        canonical.light.colorAndKind.w = kAbsoluteWorldKind;
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

    bool SnapshotEngineLight(
        const void* identity,
        ObservedLight& observed) noexcept
    {
        if (!identity) return false;

        const auto* lightBytes = static_cast<const std::byte*>(identity);
        const void* geometry = nullptr;
        std::memcpy(&geometry, lightBytes + 0xB8, sizeof(geometry));
        if (!geometry) return false;

        const auto* geometryBytes = static_cast<const std::byte*>(geometry);
        RE::NiPoint3 worldPosition{};
        RE::NiColor gammaColor{};
        RE::NiPoint3 attenuation{};
        float radius = 0.0f;
        float intensity = 0.0f;
        float colorScale = 0.0f;
        std::memcpy(&worldPosition, geometryBytes + 0xA0, sizeof(worldPosition));
        std::memcpy(&gammaColor, geometryBytes + 0x12C, sizeof(gammaColor));
        std::memcpy(&radius, geometryBytes + 0x138, sizeof(radius));
        std::memcpy(&colorScale, geometryBytes + 0x144, sizeof(colorScale));
        std::memcpy(&attenuation, geometryBytes + 0x170, sizeof(attenuation));
        std::memcpy(&intensity, lightBytes + 0x10, sizeof(intensity));

        const float combinedScale = intensity * colorScale;
        auto& light = observed.light;
        light.positionAndRadius = {
            worldPosition.x,
            worldPosition.y,
            worldPosition.z,
            radius
        };
        light.colorAndKind = {
            std::pow((std::max)(0.0f, gammaColor.r), 2.2f) * combinedScale,
            std::pow((std::max)(0.0f, gammaColor.g), 2.2f) * combinedScale,
            std::pow((std::max)(0.0f, gammaColor.b), 2.2f) * combinedScale,
            kAbsoluteWorldKind
        };
        light.attenuation = {
            attenuation.x,
            attenuation.y,
            attenuation.z,
            0.0f
        };
        observed.nativeRadius = radius;
        return IsValidLocalLight(light);
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

    struct PublicationDistance
    {
        float outsideInfluence = (std::numeric_limits<float>::max)();
        float center = (std::numeric_limits<float>::max)();
    };

    PublicationDistance DistanceFromCamera(
        const GpuLocalLight& light) noexcept
    {
        const auto& position = light.positionAndRadius;
        float deltaX = position.x;
        float deltaY = position.y;
        float deltaZ = position.z;
        if (light.colorAndKind.w > 0.5f) {
            const auto& camera =
                g_customBufferData.g_CurrentCameraPositionAdjust;
            if (!Finite(camera.x) || !Finite(camera.y) || !Finite(camera.z)) {
                return {};
            }
            deltaX -= camera.x;
            deltaY -= camera.y;
            deltaZ -= camera.z;
        }

        const float distanceSquared =
            deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
        if (!Finite(distanceSquared) || distanceSquared < 0.0f) {
            return {};
        }
        const float center = std::sqrt(distanceSquared);
        return {
            (std::max)(0.0f, center - position.w),
            center
        };
    }

    bool PublicationOrder(
        const PublicationCandidate& left,
        const PublicationCandidate& right) noexcept
    {
        // Native raster/tiled submission is view dependent. Ranking records
        // seen this frame ahead of retained records therefore changed the GPU
        // subset whenever the camera crossed a frustum boundary. Distance to
        // the light's influence sphere is stable under camera rotation and is
        // the correct first-order relevance metric for a screen-space pass.
        const auto& leftLight = left.observation.light;
        const auto& rightLight = right.observation.light;
        const auto leftDistance = DistanceFromCamera(leftLight);
        const auto rightDistance = DistanceFromCamera(rightLight);
        const float leftRadius = (std::max)(1.0f, leftLight.positionAndRadius.w);
        const float rightRadius = (std::max)(1.0f, rightLight.positionAndRadius.w);
        const float leftNormalizedOutside =
            leftDistance.outsideInfluence / leftRadius;
        const float rightNormalizedOutside =
            rightDistance.outsideInfluence / rightRadius;
        const float leftFalloff = 1.0f + leftNormalizedOutside;
        const float rightFalloff = 1.0f + rightNormalizedOutside;
        const float leftRelevance =
            PublicationScore(leftLight) / (leftFalloff * leftFalloff);
        const float rightRelevance =
            PublicationScore(rightLight) / (rightFalloff * rightFalloff);
        if (leftRelevance != rightRelevance) {
            return leftRelevance > rightRelevance;
        }
        if (leftDistance.center != rightDistance.center) {
            return leftDistance.center < rightDistance.center;
        }

        // Intensity and reach are secondary only. They break ties between
        // spatially equivalent candidates without making camera-facing cull
        // order part of the publication contract.
        const float leftScore = PublicationScore(leftLight);
        const float rightScore = PublicationScore(rightLight);
        if (leftScore != rightScore) {
            return leftScore > rightScore;
        }

        const auto& a = leftLight.positionAndRadius;
        const auto& b = rightLight.positionAndRadius;
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        if (a.w != b.w) return a.w < b.w;
        const auto& ac = leftLight.colorAndKind;
        const auto& bc = rightLight.colorAndKind;
        if (ac.x != bc.x) return ac.x < bc.x;
        if (ac.y != bc.y) return ac.y < bc.y;
        if (ac.z != bc.z) return ac.z < bc.z;
        if (ac.w != bc.w) return ac.w < bc.w;
        const auto& aa = leftLight.attenuation;
        const auto& ba = rightLight.attenuation;
        if (aa.x != ba.x) return aa.x < ba.x;
        if (aa.y != ba.y) return aa.y < ba.y;
        return aa.z < ba.z;
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
    g_lastStatsFrame = 0;
    g_uploadDirty = true;
    g_overflowLogged = false;
    g_statsLogged = false;
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
    g_overflowLogged = false;
    g_statsLogged = false;
}

void OnRasterLight(
    const void* identity,
    const RE::NiPoint3* worldPosition,
    float adjustedRadius,
    float nativeRadius,
    const RE::NiColor* linearColor,
    const RE::NiPoint3* attenuation)
{
    if (!SHADERENGINE_EFFECTS_ON || !identity || !worldPosition ||
        !linearColor || !attenuation) {
        return;
    }

    ObservedLight observed{};
    auto& light = observed.light;
    light.positionAndRadius = {
        worldPosition->x,
        worldPosition->y,
        worldPosition->z,
        adjustedRadius
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
        LightSorter::IsShadowMappedLight(identity) ? 0.0f :
            kVoxelShadowEligible
    };
    if (!IsValidLocalLight(light)) {
        return;
    }
    observed.nativeRadius =
        Finite(nativeRadius) && nativeRadius > 1.0e-3f ?
            nativeRadius : adjustedRadius;

    std::lock_guard lock(g_mutex);
    g_rasterLights[reinterpret_cast<std::uintptr_t>(identity)] = {
        observed,
        g_frame
    };
    g_uploadDirty = true;
}

void OnTiledLight(
    const RE::NiPoint3* viewPosition,
    float adjustedRadius,
    float nativeRadius,
    const RE::NiColor* color,
    const RE::NiPoint3* attenuation)
{
    if (!SHADERENGINE_EFFECTS_ON || !viewPosition || !color || !attenuation) {
        return;
    }

    ObservedLight observed{};
    auto& light = observed.light;
    light.positionAndRadius = {
        viewPosition->x,
        viewPosition->y,
        viewPosition->z,
        adjustedRadius
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
        kVoxelShadowEligible
    };
    if (!IsValidLocalLight(light)) {
        return;
    }
    observed.nativeRadius =
        Finite(nativeRadius) && nativeRadius > 1.0e-3f ?
            nativeRadius : adjustedRadius;

    std::lock_guard lock(g_mutex);
    const auto duplicate = std::find_if(
        g_tiledLights.begin(),
        g_tiledLights.end(),
        [&observed](const ObservedLight& candidate) {
            return
                std::memcmp(
                    &candidate.light,
                    &observed.light,
                    sizeof(observed.light)) == 0 &&
                candidate.nativeRadius == observed.nativeRadius;
        });
    // MAX_LIGHTS is the native GPU publication ceiling, not a collection
    // limit. Collect everything first so native submission order cannot
    // decide which records survive publication.
    if (duplicate == g_tiledLights.end()) {
        g_tiledLights.push_back(observed);
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
            std::vector<ObservedLight> transientTiled;
            transientTiled.reserve(g_tiledLights.size());
            for (const auto& tiled : g_tiledLights) {
                ObservedLight canonical{};
                if (!CanonicalizeTiledLight(tiled, canonical)) {
                    transientTiled.push_back(tiled);
                    continue;
                }

                const DirectX::XMFLOAT3 absolutePosition{
                    canonical.light.positionAndRadius.x,
                    canonical.light.positionAndRadius.y,
                    canonical.light.positionAndRadius.z
                };
                const auto retained = std::find_if(
                    g_tiledFallbackLights.begin(),
                    g_tiledFallbackLights.end(),
                    [&](const RasterLight& entry) {
                        return MatchesAbsoluteLight(
                            canonical,
                            absolutePosition,
                            entry.observation);
                    });
                if (retained != g_tiledFallbackLights.end()) {
                    retained->observation = canonical;
                    retained->lastSeenFrame = g_frame;
                } else {
                    g_tiledFallbackLights.push_back({ canonical, g_frame });
                }
            }

            // Build the complete deduplicated CPU candidate set before the
            // native 625-record publication ceiling. Publication favors
            // strong lights whose influence intersects the camera region,
            // then uses camera distance and stable attributes to break ties;
            // freshness remains an expiration concern, not a ranking input.
            std::vector<std::pair<std::uintptr_t, RasterLight>> raster;
            raster.reserve(g_rasterLights.size());
            for (const auto& entry : g_rasterLights) {
                raster.push_back(entry);
            }

            // Raster setup carries the authoritative engine shadow-array
            // classification. A matching tiled observation must not promote
            // an engine-shadowed raster light back into voxel eligibility.
            // Tiled-only records remain eligible through their own metadata.

            std::vector<PublicationCandidate> candidates;
            candidates.reserve(
                raster.size() + g_tiledFallbackLights.size() +
                transientTiled.size());
            for (const auto& entry : raster) {
                candidates.push_back({ entry.second.observation });
            }
            for (const auto& entry : g_tiledFallbackLights) {
                const auto& position =
                    entry.observation.light.positionAndRadius;
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
                            entry.observation,
                            absolutePosition,
                            rasterEntry.second.observation);
                    });
                if (!duplicate) {
                    candidates.push_back({ entry.observation });
                }
            }
            // Matrix data is unavailable only during startup. Preserve those
            // current-frame records without attempting persistence.
            for (const auto& entry : transientTiled) {
                candidates.push_back({ entry });
            }

            // Shadow-map lights can bypass SetupPointLightGeometry while a
            // matching tiled observation still reaches the bridge. Snapshot
            // the engine's authoritative shadow array and correlate by the
            // same position/radius/color/attenuation identity used for tiled
            // deduplication. This closes the last double-darkening path.
            std::array<void*, MAX_LIGHTS> shadowPointers{};
            const std::size_t shadowPointerCount =
                LightSorter::CopyShadowLightPointers(
                    shadowPointers.data(), shadowPointers.size());
            std::vector<ObservedLight> engineShadowLights;
            engineShadowLights.reserve(shadowPointerCount);
            for (std::size_t index = 0; index < shadowPointerCount; ++index) {
                ObservedLight shadowLight{};
                if (SnapshotEngineLight(shadowPointers[index], shadowLight)) {
                    engineShadowLights.push_back(shadowLight);
                }
            }
            CorrelationStats correlationStats{};
            for (auto& candidate : candidates) {
                auto& candidateLight = candidate.observation.light;
                if (candidateLight.attenuation.w < 0.5f ||
                    candidateLight.colorAndKind.w <= 0.5f) {
                    continue;
                }
                const auto& position = candidateLight.positionAndRadius;
                const DirectX::XMFLOAT3 absolutePosition{
                    position.x,
                    position.y,
                    position.z
                };
                if (CorrelateEngineShadow(
                        candidate.observation,
                        absolutePosition,
                        engineShadowLights,
                        correlationStats)) {
                    candidateLight.attenuation.w = 0.0f;
                }
            }

            std::sort(
                candidates.begin(),
                candidates.end(),
                PublicationOrder);
            if (candidates.size() > upload.size() && !g_overflowLogged) {
                REX::WARN(
                    "LocalLightBridge: {} valid local lights exceed the "
                    "native 625-light publication contract; publishing the "
                    "nearest {} by influence distance",
                    candidates.size(),
                    upload.size());
                g_overflowLogged = true;
            }
            const std::size_t count = (std::min)(
                candidates.size(),
                upload.size());
            for (std::size_t index = 0; index < count; ++index) {
                upload[index] = candidates[index].observation.light;
            }
            if (count > 0 &&
                (!g_statsLogged || g_frame - g_lastStatsFrame >= 600)) {
                std::size_t eligibleCount = 0;
                std::size_t engineShadowedCount = 0;
                for (std::size_t index = 0; index < count; ++index) {
                    if (upload[index].attenuation.w >= 0.5f) {
                        ++eligibleCount;
                    } else {
                        ++engineShadowedCount;
                    }
                }
                REX::INFO(
                    "LocalLightBridge: published {} light(s): {} voxel-eligible, "
                    "{} protected engine-shadowed; engine shadow-array={}, "
                    "snapshotted={}, correlated={}, raster cache={}, "
                    "tiled fallback={}; correlation candidates={}, "
                    "position={}, radius={}, unique={}, ambiguous={}",
                    count,
                    eligibleCount,
                    engineShadowedCount,
                    LightSorter::GetShadowLightCount(),
                    engineShadowLights.size(),
                    correlationStats.uniqueMatches,
                    raster.size(),
                    g_tiledFallbackLights.size(),
                    correlationStats.eligibleCandidates,
                    correlationStats.positionMatches,
                    correlationStats.radiusMatches,
                    correlationStats.uniqueMatches,
                    correlationStats.ambiguousMatches);
                g_lastStatsFrame = g_frame;
                g_statsLogged = true;
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
