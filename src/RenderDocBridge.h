#pragma once

// In-process RenderDoc capture, following Community Shaders' RenderDoc
// feature (Bottle branch). There is no injector: having renderdoc.dll loaded
// in the process before the D3D11 device is created makes RenderDoc hook the
// API, and RENDERDOC_GetAPI then provides programmatic control.
//
// The enable switch is FILE PRESENCE: renderdoc.dll sitting next to
// ShaderEngineCL.dll arms capture at launch; remove or rename it and the
// game runs completely clean. No INI, no GUI state to forget.
//
// Capturing: RenderDoc's default hotkeys stay active (F12 / PrintScreen),
// and its overlay stays visible as confirmation that capture is armed.
// Capture files land under the path logged at startup.
namespace RenderDocBridge
{
    // Must run before D3D11 device creation - call from plugin load.
    void Initialize();
}
