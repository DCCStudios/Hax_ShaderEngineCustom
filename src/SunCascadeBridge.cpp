#include <PCH.h>
#include "SunCascadeBridge.h"

#include "Global.h"
#include "RenderTargets.h"
#include "ShadowTelemetry.h"

namespace SunCascadeBridge
{
namespace
{
    // Mirrors the HLSL SESunCascade declaration in skylightingCommon.inc.
    // 4x4 transform plus one info vector = 80 bytes, 16-byte aligned.
    struct GpuSunCascade
    {
        float transform[16] = {};
        // x = 1 when this cascade is live, y = shadow texture array slice,
        // z = number of live cascades this frame, w = reserved.
        float info[4] = {};
    };
    static_assert(sizeof(GpuSunCascade) == 80, "SESunCascade layout must match HLSL");

    REX::W32::ID3D11Buffer*             g_buffer = nullptr;
    REX::W32::ID3D11ShaderResourceView* g_srv    = nullptr;

    REX::W32::ID3D11ShaderResourceView* g_shadowArraySrv     = nullptr;
    REX::W32::ID3D11Texture2D*          g_shadowArraySource  = nullptr;

    // Builds (or rebuilds) a TEXTURE2DARRAY view over the shadow map array.
    // Tracks the source texture pointer so a renderer resize, which reallocates
    // the target, produces a fresh view instead of a dangling one.
    REX::W32::ID3D11ShaderResourceView* EnsureShadowArraySrv(
        REX::W32::ID3D11Device* device)
    {
        if (!device || !g_rendererData) {
            return nullptr;
        }
        auto* texture = g_rendererData
            ->depthStencilTargets[RT::idx(RT::Depth::kShadowMap)].texture;
        if (!texture) {
            return nullptr;
        }
        if (g_shadowArraySrv && g_shadowArraySource == texture) {
            return g_shadowArraySrv;
        }

        if (g_shadowArraySrv) {
            g_shadowArraySrv->Release();
            g_shadowArraySrv = nullptr;
        }
        g_shadowArraySource = texture;

        REX::W32::D3D11_TEXTURE2D_DESC textureDesc{};
        texture->GetDesc(&textureDesc);

        // The shadow target is typeless depth; pick the matching depth-read
        // format so the view returns normalised depth rather than failing.
        REX::W32::DXGI_FORMAT readFormat = REX::W32::DXGI_FORMAT_UNKNOWN;
        switch (textureDesc.format) {
        case REX::W32::DXGI_FORMAT_R24G8_TYPELESS:
        case REX::W32::DXGI_FORMAT_D24_UNORM_S8_UINT:
            readFormat = REX::W32::DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            break;
        case REX::W32::DXGI_FORMAT_R32_TYPELESS:
        case REX::W32::DXGI_FORMAT_D32_FLOAT:
            readFormat = REX::W32::DXGI_FORMAT_R32_FLOAT;
            break;
        case REX::W32::DXGI_FORMAT_R16_TYPELESS:
        case REX::W32::DXGI_FORMAT_D16_UNORM:
            readFormat = REX::W32::DXGI_FORMAT_R16_UNORM;
            break;
        default:
            readFormat = textureDesc.format;
            break;
        }

        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.format                         = readFormat;
        desc.viewDimension                  = REX::W32::D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.texture2DArray.mostDetailedMip = 0;
        desc.texture2DArray.mipLevels       = 1;
        desc.texture2DArray.firstArraySlice = 0;
        desc.texture2DArray.arraySize       = textureDesc.arraySize;

        const HRESULT hr =
            device->CreateShaderResourceView(texture, &desc, &g_shadowArraySrv);
        if (FAILED(hr)) {
            REX::WARN(
                "SunCascadeBridge: shadow array SRV failed (format {} -> {}, "
                "slices {}, HRESULT 0x{:08X})",
                static_cast<int>(textureDesc.format),
                static_cast<int>(readFormat),
                textureDesc.arraySize,
                static_cast<std::uint32_t>(hr));
            g_shadowArraySrv = nullptr;
            return nullptr;
        }
        REX::INFO(
            "SunCascadeBridge: shadow array SRV created ({}x{}, {} slices, "
            "format {} -> {})",
            textureDesc.width, textureDesc.height, textureDesc.arraySize,
            static_cast<int>(textureDesc.format),
            static_cast<int>(readFormat));
        return g_shadowArraySrv;
    }

    bool EnsureGpuResource(REX::W32::ID3D11Device* device)
    {
        if (!device) {
            return false;
        }

        if (!g_buffer) {
            REX::W32::D3D11_BUFFER_DESC desc{};
            desc.usage               = REX::W32::D3D11_USAGE_DEFAULT;
            desc.byteWidth           = sizeof(GpuSunCascade) * MAX_CASCADES;
            desc.bindFlags           = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            desc.miscFlags           = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.structureByteStride = sizeof(GpuSunCascade);
            const HRESULT hr = device->CreateBuffer(&desc, nullptr, &g_buffer);
            if (FAILED(hr)) {
                REX::WARN(
                    "SunCascadeBridge: failed to create cascade buffer (HRESULT 0x{:08X})",
                    static_cast<std::uint32_t>(hr));
                return false;
            }
        }

        if (!g_srv) {
            REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.format              = REX::W32::DXGI_FORMAT_UNKNOWN;
            desc.viewDimension       = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
            desc.buffer.firstElement = 0;
            desc.buffer.numElements  = MAX_CASCADES;
            const HRESULT hr = device->CreateShaderResourceView(g_buffer, &desc, &g_srv);
            if (FAILED(hr)) {
                REX::WARN(
                    "SunCascadeBridge: failed to create t{} SRV (HRESULT 0x{:08X})",
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
    if (g_shadowArraySrv) {
        g_shadowArraySrv->Release();
        g_shadowArraySrv = nullptr;
    }
    g_shadowArraySource = nullptr;
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
    if (!context || !SUN_CASCADE_CAPTURE_ON) {
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

    GpuSunCascade upload[MAX_CASCADES]{};

    // A failed validity check publishes zero live cascades rather than
    // suspect matrices. Consuming shaders read info.z == 0 as "no sun shadow
    // data" and fall back to fully lit, never to fully shadowed.
    ShadowTelemetry::DirectionalCascade captured[ShadowTelemetry::kMaxPublishedCascades]{};
    const std::uint32_t published =
        ShadowTelemetry::SunCascadesLookValid()
            ? ShadowTelemetry::GetDirectionalCascades(captured, MAX_CASCADES)
            : 0u;

    std::uint32_t liveCount = 0;
    for (std::uint32_t i = 0; i < MAX_CASCADES; ++i) {
        const bool live = (i < published) && captured[i].valid;
        if (live) {
            std::memcpy(upload[i].transform, captured[i].transform,
                        sizeof(upload[i].transform));
            liveCount = i + 1u;
        }
        upload[i].info[0] = live ? 1.0f : 0.0f;
        upload[i].info[1] = live ? static_cast<float>(captured[i].mapSlot) : 0.0f;
        upload[i].info[3] = 0.0f;
    }
    for (auto& entry : upload) {
        entry.info[2] = static_cast<float>(liveCount);
    }

    context->UpdateSubresource(g_buffer, 0, nullptr, upload, 0, 0);

    REX::W32::ID3D11Device* srvDevice = nullptr;
    context->GetDevice(&srvDevice);
    REX::W32::ID3D11ShaderResourceView* shadowArray =
        EnsureShadowArraySrv(srvDevice);
    if (srvDevice) {
        srvDevice->Release();
    }

    if (pixelStage) {
        context->PSSetShaderResources(SRV_SLOT, 1, &g_srv);
        if (shadowArray) {
            context->PSSetShaderResources(SHADOW_ARRAY_SLOT, 1, &shadowArray);
        }
    } else {
        context->CSSetShaderResources(SRV_SLOT, 1, &g_srv);
        if (shadowArray) {
            context->CSSetShaderResources(SHADOW_ARRAY_SLOT, 1, &shadowArray);
        }
    }
}
}
