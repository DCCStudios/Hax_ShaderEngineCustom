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

}  // namespace ShadowUpgrade
