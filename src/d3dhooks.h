#pragma once

// Windows SDK D3D11 declarations for the early D3D11CreateDeviceAndSwapChain hook.
#include <d3d11.h>

#include <PCH.h>

// Refreshes GFXInjected once at the first custom-pass draw boundary between
// two Presents. This gives screen-space tracing the camera matrices that
// produced the current depth buffer instead of the preceding frame's data.
void RefreshCustomBufferForCustomPass();

namespace D3D11Hooks
{
    using D3D11CreateDeviceAndSwapChain_t = HRESULT(WINAPI*)(
        IDXGIAdapter*,
        D3D_DRIVER_TYPE,
        HMODULE,
        UINT,
        const D3D_FEATURE_LEVEL*,
        UINT,
        UINT,
        const DXGI_SWAP_CHAIN_DESC*,
        IDXGISwapChain**,
        ID3D11Device**,
        D3D_FEATURE_LEVEL*,
        ID3D11DeviceContext**);
    using ClipCursor_t = BOOL(WINAPI*)(const RECT*);

    using Present_t = HRESULT(STDMETHODCALLTYPE*)(
        REX::W32::IDXGISwapChain*,
        UINT,
        UINT);
    using PSSetShaderResources_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        UINT,
        UINT,
        REX::W32::ID3D11ShaderResourceView* const*);
    using OMSetRenderTargets_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        UINT,
        REX::W32::ID3D11RenderTargetView* const*,
        REX::W32::ID3D11DepthStencilView*);
    using ClearDepthStencilView_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        REX::W32::ID3D11DepthStencilView*,
        UINT,
        FLOAT,
        UINT8);
    using DrawIndexed_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        UINT,
        UINT,
        INT);
    using Draw_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        UINT,
        UINT);
    using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        UINT,
        UINT,
        UINT,
        INT,
        UINT);
    using DrawInstanced_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        UINT,
        UINT,
        UINT,
        UINT);
    using Map_t = HRESULT(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        REX::W32::ID3D11Resource*,
        UINT,
        REX::W32::D3D11_MAP,
        UINT,
        REX::W32::D3D11_MAPPED_SUBRESOURCE*);
    using Unmap_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        REX::W32::ID3D11Resource*,
        UINT);
    using SetConstantBuffers_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        UINT,
        UINT,
        REX::W32::ID3D11Buffer* const*);
    using SetSamplers_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        UINT,
        UINT,
        REX::W32::ID3D11SamplerState* const*);
    using IASetInputLayout_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        REX::W32::ID3D11InputLayout*);
    using IASetVertexBuffers_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        UINT,
        UINT,
        REX::W32::ID3D11Buffer* const*,
        const UINT*,
        const UINT*);
    using IASetIndexBuffer_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        REX::W32::ID3D11Buffer*,
        REX::W32::DXGI_FORMAT,
        UINT);
    using SetState1_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        void*);
    using OMSetBlendState_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        REX::W32::ID3D11BlendState*,
        const FLOAT[4],
        UINT);
    using OMSetDepthStencilState_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        REX::W32::ID3D11DepthStencilState*,
        UINT);
    using PSSetShader_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        REX::W32::ID3D11PixelShader*,
        REX::W32::ID3D11ClassInstance* const*,
        UINT);
    using VSSetShader_t = void(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11DeviceContext*,
        REX::W32::ID3D11VertexShader*,
        REX::W32::ID3D11ClassInstance* const*,
        UINT);
    using CreatePixelShader_t = HRESULT(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11Device*,
        const void*,
        SIZE_T,
        REX::W32::ID3D11ClassLinkage*,
        REX::W32::ID3D11PixelShader**);
    using CreateVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(
        REX::W32::ID3D11Device*,
        const void*,
        SIZE_T,
        REX::W32::ID3D11ClassLinkage*,
        REX::W32::ID3D11VertexShader**);

    extern D3D11CreateDeviceAndSwapChain_t OriginalD3D11CreateDeviceAndSwapChain;
    extern ClipCursor_t OriginalClipCursor;
    extern Present_t OriginalPresent;
    extern PSSetShaderResources_t OriginalPSSetShaderResources;
    extern OMSetRenderTargets_t OriginalOMSetRenderTargets;
    extern ClearDepthStencilView_t OriginalClearDepthStencilView;
    extern DrawIndexed_t OriginalDrawIndexed;
    extern Draw_t OriginalDraw;
    extern DrawIndexedInstanced_t OriginalDrawIndexedInstanced;
    extern DrawInstanced_t OriginalDrawInstanced;
    extern Map_t OriginalMap;
    extern Unmap_t OriginalUnmap;
    extern SetConstantBuffers_t OriginalVSSetConstantBuffers;
    extern SetConstantBuffers_t OriginalPSSetConstantBuffers;
    extern SetSamplers_t OriginalPSSetSamplers;
    extern IASetInputLayout_t OriginalIASetInputLayout;
    extern IASetVertexBuffers_t OriginalIASetVertexBuffers;
    extern IASetIndexBuffer_t OriginalIASetIndexBuffer;
    extern OMSetBlendState_t OriginalOMSetBlendState;
    extern OMSetDepthStencilState_t OriginalOMSetDepthStencilState;
    extern SetState1_t OriginalRSSetState;
    extern PSSetShader_t OriginalPSSetShader;
    extern VSSetShader_t OriginalVSSetShader;
    extern CreatePixelShader_t OriginalCreatePixelShader;
    extern CreateVertexShader_t OriginalCreateVertexShader;

    void EnsureDrawHooksPresent();
    void ResetCommandBufferReplayState();
}
