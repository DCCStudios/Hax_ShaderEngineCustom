# Custom Render Passes & Custom Resources

This document specifies an extension to the existing `Shader.ini` schema that lets a
shader package declare:

- Persistent textures (`customResource`) with stable SRV slot bindings, optional UAVs, and
  per-frame copy/clear/pingpong rules.
- Full-screen pixel-shader passes and compute-shader dispatches (`customPass`) that fire at
  user-chosen trigger points inside the engine's frame.

The goal is to lift the plugin from a pure shader-replacement harness into one that can host
multi-pass effects (SSRTGI, ReSTIR-style screen-space resampling, two-pass denoisers, etc.)
without modifying the engine's render graph.

The new section blocks live alongside existing `[shaderId]` blocks in the same `Shader.ini`
files and follow the same `[name]…[/name]` syntax.

---

## `[customResource:NAME]`

Declares one persistent GPU resource owned by the plugin. Resources are allocated lazily on
first use and survive across frames.

```ini
[customResource:giCurrent]
; --- Format & geometry ---
format=R11G11B10_FLOAT      ; DXGI_FORMAT_*; R11G11B10_FLOAT, R16G16B16A16_FLOAT,
                            ;                R8G8B8A8_UNORM, R32_FLOAT, etc.
scale=screen/2              ; "screen" | "screen/N" | "WxH"
domain=allocation           ; "allocation" (default) | "render"
mipLevels=1                 ; default 1
; --- Bindings ---
srvSlot=40                  ; t-slot bound globally to replacement PS/VS shaders.
                            ; Prefer t40+ for custom resources.
                            ; -1 disables global binding (still usable by name).
uav=true                    ; allocate a UAV (required for compute writes)
rtv=true                    ; allocate an RTV (required for PS writes)
; --- Per-frame behavior ---
clearOnPresent=false        ; clear to clearColor in the Present hook before any pass
clearColor=0,0,0,0
copyFrom=                   ; "currentRTV" | "renderTargets[N]" | "" (none)
copyAt=                     ; "present" | "" (none)
persistent=true             ; if false, resource is recreated on each pass fire
[/customResource:giCurrent]
```

`domain=allocation` resolves `screen` against the currently available
`renderTargets[3]` allocation. `domain=render` instead resolves it against the
logical projection extent injected by ShaderEngine. Use the render domain for
screen-space history and ray-tracing fields: under an external upscaler some
engine textures (notably RT39, the depth pyramid) retain display-sized
allocations even though the G-buffer projection is reduced.

### `pingpong` rule

Two resources can be declared as a ping-pong pair so reads/writes flip every frame:

```ini
[customResource:giHistory]
format=R11G11B10_FLOAT
scale=screen/2
srvSlot=41
uav=true
rtv=true
pingpongWith=giCurrent      ; symmetric — declare on either side
[/customResource:giHistory]
```

At end-of-frame the plugin swaps the two resources' underlying texture pointers (and their
SRV/RTV/UAV views), so a shader that sampled `giHistory` on frame N will see what was
written to `giCurrent` on frame N-1.

---

## `[customPass:NAME]`

Declares a pass to dispatch. The pass is a single full-screen PS draw or compute dispatch.

```ini
[customPass:hachiSSRTGI]
active=true
priority=0

; --- Trigger ---
; The pass fires at the indicated point. Trigger options (mutually exclusive):
;   triggerShaderUID=PSXXXXX     fire on raw UID match — UNSAFE: Bethesda's
;                                32-bit asmHash component collides across
;                                unrelated shaders, so this can fire your pass
;                                during unintended draws (bloom blur, etc.)
;   triggerHookId=visualTonemap  fire before the actual shader matched by an
;                                existing [shaderId] block. Resolves through
;                                ShaderEngine's full matcher (UID + size +
;                                buffersize + textures + IO masks), so it is
;                                collision-proof. PREFER THIS.
;   triggerAtPresent=true        fire each frame inside Present, before ImGui
trigger=beforeHookId:visualTonemap
oncePerFrame=true            ; default true; false = fire every match (rare, debug only)

; --- Shader ---
type=ps                      ; ps | cs
shader=visualSSRTGI.hlsl     ; HLSL source, resolved relative to the package folder

; For type=cs only:
;   threadGroups=screen/8,screen/8,1     dispatch geometry; "screen[/N]" or literal int
;   entry=main                            entry point name (default "main")

; --- Inputs (SRVs) ---
; slot=source where source is one of:
;   depth                       g_depthSRV (main scene depth)
;   currentRTV                  snapshot SRV of OMGetRenderTargets()[0] at trigger time
;   customResource:NAME         a declared customResource (read SRV)
;   gbufferRT:N                 g_rendererData->renderTargets[N].srView (explicit index)
;   sceneHDR                    renderTargets[kMain=3].srView (engine HDR scene)
;   gbufferNormal               renderTargets[NORMAL_BUFFER_INDEX].srView (default 20).
;                               Stays unbound when NORMAL_BUFFER_INDEX is -1, leaving
;                               the slot null so the consuming shader can fall back
;                               gracefully (e.g. depth-derivative reconstruction).
;   gbufferAlbedo               renderTargets[kGbufferAlbedo=22].srView
;   gbufferMaterial             renderTargets[kGbufferMaterial=24].srView
;                               (.x glossiness, .y specular, .z backlighting, .w SSS)
;   motionVectors               renderTargets[kMotionVectors=29].srView
input=0:sceneHDR,1:depth,2:customResource:giHistory,3:gbufferNormal,4:gbufferAlbedo,5:motionVectors

; --- Outputs ---
; For type=ps:  slot=customResource:NAME (RTV)
; For type=cs:  uav=slot:customResource:NAME (UAV)
output=0:customResource:giCurrent

; --- Render state (PS only) ---
viewport=screen/2            ; matches the output's scale by default
clearOnFire=false
blend=opaque                 ; opaque | additive | premulalpha | multiply
depthTest=false

; --- Logging ---
log=false
profileGpu=false              ; asynchronous timestamps shown in the Custom Passes overlay
[/customPass:hachiSSRTGI]
```

`profileGpu=true` wraps only the pass draw or dispatch in a four-slot D3D11
timestamp ring. Results are polled at least two frames later with
`D3D11_ASYNC_GETDATA_DONOTFLUSH`; an unresolved result is skipped rather than
waiting for the GPU. The overlay reports the most recent sample and an EMA.

### Trigger semantics

- `beforeShaderUID:UID` and `beforeHookId:ID`: the pass fires inside the existing
  `MyPSSetShader` hook, *immediately before* the original engine shader's `PSSetShader`
  swap takes effect. `oncePerFrame=true` (default) ensures it does not re-fire if the same
  shader is set multiple times in a single frame.
- `atPresent`: the pass fires inside `MyPresent`, before any ImGui drawing, after the
  per-frame `customResource` `copyAt=present` operations.

### State save/restore

Around every pass the plugin saves and restores: viewport, scissor, OM render targets +
depth-stencil view, blend state, depth-stencil state, rasterizer state, IA primitive
topology + vertex/index buffers + input layout, all VS/PS shader bindings, the first 16
PS SRVs.

### Built-in fullscreen vertex shader

`type=ps` passes don't need a vertex shader in the package. The plugin compiles a
shared `FullscreenTriangleVS` once and binds it for all PS-typed customPass draws. The PS
gets `float4 SV_POSITION` and `float2 TEXCOORD0` (UV) as input.

---

## Built-in resources & slot map

After the schema lands the plugin reserves the following slots by default
(overridable per-resource via `srvSlot=`):

| Slot | Resource                              | Notes                                  |
|------|---------------------------------------|----------------------------------------|
| t26  | `GFXDrawTag`                          | unchanged                              |
| t27  | `GFXModularBools`                     | unchanged                              |
| t28  | `GFXModularInts`                      | unchanged                              |
| t29  | `GFXModularFloats`                    | unchanged                              |
| t30  | `g_DepthSRV`                          | unchanged (main scene depth)           |
| t31  | `GFXInjected` (booster CB)            | unchanged                              |
| t40+    | preferred `customResource` SRVs     | configurable via `srvSlot`; keep package-wide |

`customResource` blocks with `srvSlot >= 0` cause the plugin to publish that SRV to
replacement pixel and vertex shaders on the declared slot. The implementation does not
clamp this to a fixed range. Prefer `t40+` for new resources because it avoids the
plugin's built-in injected SRVs (`t26-t31`) and leaves room for package-defined
resources. Existing replacement shaders can sample them by name once the slot is declared
in the package's HLSL.

---

## Examples

### Single-pass SSRTGI

```ini
[customResource:giCurrent]
format=R11G11B10_FLOAT
scale=screen/2
srvSlot=40
rtv=true
pingpongWith=giHistory
[/customResource:giCurrent]

[customResource:giHistory]
format=R11G11B10_FLOAT
scale=screen/2
srvSlot=41
rtv=true
pingpongWith=giCurrent
[/customResource:giHistory]

[customPass:hachiSSRTGI]
active=true
type=ps
shader=visualSSRTGI.hlsl
trigger=beforeShaderUID:PS9C19612AI0O1   ; visualTonemap UID
input=0:currentRTV,1:depth,2:customResource:giHistory
output=0:customResource:giCurrent
viewport=screen/2
[/customPass:hachiSSRTGI]
```

The existing `visualTonemap.hlsl` then samples `giCurrent` from t40 and adds it before
tonemap.

### Two-pass denoise (sketch)

```ini
[customResource:giDenoised]
format=R11G11B10_FLOAT
scale=screen/2
srvSlot=42
rtv=true
[/customResource:giDenoised]

[customPass:hachiSSRTGIDenoise]
active=true
type=ps
shader=visualSSRTGIDenoise.hlsl
trigger=beforeShaderUID:PS9C19612AI0O1
input=0:customResource:giCurrent,1:depth
output=0:customResource:giDenoised
[/customPass:hachiSSRTGIDenoise]
```

`visualTonemap.hlsl` then samples `giDenoised` (t42) instead of `giCurrent`.
