#pragma once

#include <PCH.h>

// Owns the one renderer-phase hook needed by the shadow upgrade. Shader
// replacement itself remains on ShaderEngine's normal ShaderDB path.
// Telemetry modules observe this scope; they do not own it.
namespace ShadowUpgrade {

// Installs the OG DrawWorld::DeferredLightsImpl wrapper after validating the
// exact IDA-confirmed prologue. Safe to call repeatedly.
bool Initialize();

// Applies settings that depend on Fallout's INIPref values after game data is
// ready. Safe to call from later load/new-game messages as a retry.
void OnGameDataReady();

// True only while the validated DeferredLightsImpl wrapper is on this thread.
bool IsInDeferredLighting() noexcept;

// Previous PS constant-buffer bindings captured for one late custom-pass
// batch. D3D11 PSGetConstantBuffers AddRefs both pointers; Restore releases
// them after rebinding, including when the previous slot was null.
struct DeferredReconstructionBinding
{
    REX::W32::ID3D11Buffer* previousB2 = nullptr;
    REX::W32::ID3D11Buffer* previousB12 = nullptr;
    bool active = false;
};

// Temporarily publishes both captured DeferredLights reconstruction snapshots:
// b2 supplies physical pixel-to-clip scale and b12 supplies the near inverse
// projection rows. Both must be valid or neither is changed. A successful bind
// must be paired with RestoreDeferredReconstructionConstants after the batch.
bool BindDeferredReconstructionConstants(
    REX::W32::ID3D11DeviceContext* context,
    DeferredReconstructionBinding& binding) noexcept;
void RestoreDeferredReconstructionConstants(
    REX::W32::ID3D11DeviceContext* context,
    DeferredReconstructionBinding& binding) noexcept;

// True only on the render thread while the paired b2+b12 scope is active.
// ContactShadowBridge publishes this as the late VM trace's independent t40.w
// reconstruction-ready bit. This prevents stale tonemap-constant reads while
// allowing local-only lighting when no directional source is available.
bool IsDeferredReconstructionBoundForCurrentThread() noexcept;

}  // namespace ShadowUpgrade
