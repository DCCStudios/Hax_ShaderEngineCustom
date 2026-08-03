#pragma once

// Named indices into RE::BSGraphics::RendererData::renderTargets[] and
// depthStencilTargets[]. Authoritative for the OG (1.10.163) runtime; the
// 984/191 layouts match for the indices we use in this plugin (G-buffer,
// scene HDR, motion, depth, SSAO).
//
// Source: community-derived enum (see DRAW_TAGGING_NOTES.md and game
// reverse-engineering threads). Keeping this in one place means the plugin
// no longer scatters magic numbers across the code.

namespace RT {

enum class Color : int
{
    kFrameBuffer = 0,

    kRefractionNormal = 1,

    kMainPreAlpha = 2,
    kMain = 3,           // main HDR scene buffer (full-screen)
    kMainTemp = 4,

    kSSRRaw = 7,
    kSSRBlurred = 8,
    kSSRBlurredExtra = 9,

    kSSRDirection = 10,
    kSSRMask = 11,

    kMainVerticalBlur = 14,
    kMainHorizontalBlur = 15,

    kUI = 17,
    kUITemp = 18,

    kGbufferNormal = 20,
    kGbufferNormalSwap = 21,
    kGbufferAlbedo = 22,
    kGbufferEmissive = 23,
    kGbufferMaterial = 24,   // glossiness, specular, backlighting, SSS

    kTAAAccumulation = 26,
    kTAAAccumulationSwap = 27,

    kSSAO = 28,
    kMotionVectors = 29,

    kUIDownscaled = 36,
    kUIDownscaledComposite = 37,

    kMainDepthMips = 39,

    kSSAOTemp = 48,
    kSSAOTemp2 = 49,
    kSSAOTemp3 = 50,

    kUnkMask = 57,

    kDiffuseBuffer = 58,
    kSpecularBuffer = 59,

    kDownscaledHDR = 64,
    kDownscaledHDRLuminance2 = 65,
    kDownscaledHDRLuminance3 = 66,
    kDownscaledHDRLuminance4 = 67,
    kDownscaledHDRLuminance5Adaptation = 68,
    kDownscaledHDRLuminance6AdaptationSwap = 69,
    kDownscaledHDRLuminance6 = 70,

    kCount = 101
};

enum class Depth : int
{
    kMainOtherOther = 0,
    kMainOther = 1,
    kMain = 2,
    kMainCopy = 3,
    kMainCopyCopy = 4,

    // OG IDA: BSGraphics::RenderTargetManager::QShadowMapArrayDepthStencil
    // @ 0x141D31770 returns logical depth target 6. Live directional and
    // BSShadowFrustumLight maps both use this array and reuse its slices.
    kShadowMap = 6,

    kCount = 13
};

constexpr int idx(Color c) { return static_cast<int>(c); }
constexpr int idx(Depth d) { return static_cast<int>(d); }

}  // namespace RT
