#pragma once

#include <PCH.h>

struct ShaderDefinition;
class GlobalShaderSettings;

namespace ShaderResources
{
    enum class DepthStencilTarget : UINT
    {
        kMainOtherOther = 0,
        kMainOther = 1,
        kMain = 2,
        kMainCopy = 3,
        kMainCopyCopy = 4,
        // OG QShadowMapArrayDepthStencil @ 0x141D31770 returns target 6.
        kShadowMap = 6,
        kCount = 13
    };

    inline constexpr UINT DEPTHBUFFER_SLOT = 30;
    inline constexpr auto MAIN_DEPTHSTENCIL_TARGET = DepthStencilTarget::kMain;
    inline constexpr UINT DEPTHSTENCIL_TARGET_COUNT = static_cast<UINT>(DepthStencilTarget::kCount);

    void ReleaseSRVBuffer(REX::W32::ID3D11Buffer*& buffer, REX::W32::ID3D11ShaderResourceView*& srv);

    void PackModularShaderValues(GlobalShaderSettings& settings);
    void EnsureInjectedShaderResourceViews(REX::W32::ID3D11Device* device);
    void UpdateInjectedShaderResourceViews(REX::W32::ID3D11DeviceContext* context);

    void BindInjectedPixelShaderResources(REX::W32::ID3D11DeviceContext* context);
    void BindInjectedVertexShaderResources(REX::W32::ID3D11DeviceContext* context);
    void BindReplacementTextureResources(
        REX::W32::ID3D11DeviceContext* context,
        ShaderDefinition* def,
        bool pixelStage);
    void BindReplacementSRVResources(
        REX::W32::ID3D11DeviceContext* context,
        ShaderDefinition* def,
        bool pixelStage);

    REX::W32::ID3D11ShaderResourceView* GetDepthBufferSRV_Internal();
    UINT FindDepthTargetIndexForDSV(REX::W32::ID3D11DepthStencilView* dsv);
    void TrackOMRenderTargets(
        UINT numViews,
        REX::W32::ID3D11RenderTargetView* const* renderTargetViews,
        REX::W32::ID3D11DepthStencilView* depthStencilView);

    UINT GetCurrentDepthTargetIndex() noexcept;
    bool HasCurrentRenderTarget() noexcept;

    bool ActiveReplacementPixelShaderActive() noexcept;
    bool ActiveReplacementPixelShaderUsesDrawTag() noexcept;
    bool ActiveReplacementPixelShaderNeedsResourceRebind() noexcept;
    void SetActiveReplacementPixelShaderUsage(const ShaderDefinition* def, bool active) noexcept;

    bool IsCommandBufferReplayActive() noexcept;
    void EnterCommandBufferReplay() noexcept;
    void LeaveCommandBufferReplay() noexcept;
}
