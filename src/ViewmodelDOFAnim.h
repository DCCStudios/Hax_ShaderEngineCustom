#pragma once

// Viewmodel-DOF weapon-animation suppression.
//
// Blends the viewmodel depth-of-field strength in/out based on whether a discrete
// first-person weapon animation is playing, so DOF doesn't blur the weapon while
// it swings close. Detection is game-native (SeamlessInspect's approach): the
// player's BSAnimationGraphEvent sink is hooked for reload events (reloadStart /
// reloadComplete / ReloadEnd) and IdleStop, and AIProcess::SetupSpecialIdle is
// hooked for the start of inspect/examine idles. All hooks fire on the main
// thread; smoothing runs on the render thread. The result is published to
// GFXBoosterAccessData::g_ViewmodelDOF.x and applied in visualViewmodelDOF.hlsl.

namespace ViewmodelDOFAnim
{
    // Install the animation hooks. Call once on kGameDataReady (player exists).
    void Install();

    // Render-thread entry, once per frame. Advances/returns the smoothed DOF blend
    // in [0,1]: 1 = full DOF, 0 = fully suppressed.
    float Update(float a_deltaSeconds, float a_blendTimeSeconds);
}
