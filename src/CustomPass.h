#pragma once

// Custom render passes & custom resources.
//
// Lifts the plugin from a pure shader-replacement harness to one that can host
// dedicated full-screen / compute passes that own their own persistent textures.
// See CustomPass.md for the INI schema; this header is the C++ surface used by
// the rest of the plugin.

#include <PCH.h>
#include <d3d11.h>

namespace CustomPass {

// --- Forward types --------------------------------------------------------

class Resource;
class Pass;
class Registry;
class FileWatcher;
// Per-fire D3D11 state snapshot. Definition is in CustomPass.cpp; only the
// type name is needed at the Registry method signatures for the batched
// fire path (see Registry::FireBatch).
struct SavedState;

// HLSL hot-reload watcher specialised for customPass. Mirrors the design of
// the existing HlslFileWatcher (in Global.h) which is hard-tied to
// ShaderDefinition, but invokes a generic lambda on change so it can target
// any pass-bound state.
class FileWatcher {
public:
    using OnChange = std::function<void()>;
    FileWatcher(std::filesystem::path path, OnChange cb);
    ~FileWatcher();
    void Start();
    void Stop();

private:
    std::filesystem::path             filePath;
    std::filesystem::file_time_type   lastWriteTime{};
    OnChange                          onChange;
    std::atomic<bool>                 running{ false };
    std::thread                       watcherThread;
    std::mutex                        stopMutex;
    std::condition_variable           stopCv;
};

// --- Resource -------------------------------------------------------------

// Saved = the viewport captured from the engine at fire time. Only
// meaningful for a pass's `viewport=` (resources have no live viewport):
// at a beforeDrawForHook trigger the snapshot holds the hooked draw's own
// viewport, which during the imagespace HDR phase is the dynamic-resolution
// SUBRECT of the target allocation - rasterizing there makes the pass's
// fullscreen-triangle UV equal the engine's scene-logical UV.
enum class ScaleMode : uint8_t { Screen, ScreenDiv, Absolute, Saved };

struct ResourceSpec {
    std::string                             name;
    REX::W32::DXGI_FORMAT                   format = REX::W32::DXGI_FORMAT_R11G11B10_FLOAT;
    ScaleMode                               scaleMode = ScaleMode::Screen;
    uint32_t                                scaleDiv = 1;       // Screen/N
    uint32_t                                absWidth = 0;       // Absolute
    uint32_t                                absHeight = 0;      // Absolute
    uint32_t                                depth = 1;          // depth > 1 selects Texture3D
    bool                                    renderDomain = false; // scale relative to logical render extent
    uint32_t                                mipLevels = 1;
    int                                     srvSlot = -1;       // Preferred/requested SRV slot for INI consumers.
    bool                                    globalBind = false; // true publishes srvSlot without an explicit request.
    bool                                    needRtv = true;
    bool                                    needUav = false;
    bool                                    clearOnPresent = false;
    float                                   clearColor[4] = { 0, 0, 0, 0 };
    std::string                             copyFrom;           // empty | "currentRTV" | "renderTargets[N]"
    std::string                             copyAt;             // empty | "present"
    bool                                    persistent = true;
    std::string                             pingpongWith;       // partner resource name (symmetric)
};

class Resource {
public:
    ResourceSpec                                spec{};
    REX::W32::ID3D11Texture2D*                  texture = nullptr;
    REX::W32::ID3D11Texture3D*                  texture3D = nullptr;
    REX::W32::ID3D11RenderTargetView*           rtv = nullptr;
    REX::W32::ID3D11ShaderResourceView*         srv = nullptr;
    // Non-owning alias of mipUavs[0], retained for existing callers.
    REX::W32::ID3D11UnorderedAccessView*        uav = nullptr;
    std::vector<REX::W32::ID3D11UnorderedAccessView*> mipUavs;
    uint32_t                                    width = 0;
    uint32_t                                    height = 0;
    uint32_t                                    depth = 0;
    REX::W32::DXGI_FORMAT                       allocatedFormat = REX::W32::DXGI_FORMAT_UNKNOWN;
    Resource*                                   pingpongPartner = nullptr;

    // Allocate / release. Idempotent: EnsureAllocated returns true if usable.
    bool EnsureAllocated(REX::W32::ID3D11Device* device,
                         uint32_t backbufferW,
                         uint32_t backbufferH);
    void Release();

    // Swap underlying texture/view pointers with `other`. Used by ping-pong.
    void SwapContents(Resource& other);
};

// --- Pass -----------------------------------------------------------------

enum class PassType : uint8_t { Pixel, Compute };
enum class TriggerKind : uint8_t {
    None,
    BeforeShaderUID,         // raw UID match — risk of asmHash collisions
    BeforeHookId,             // resolves to BeforeMatchedDefinition after ResolveHookIdTriggers()
    BeforeMatchedDefinition, // fires inside MyPSSetShader, before the engine swaps to the
                              // new shader. State seen at this moment is STALE from the
                              // previous draw — the engine sets the new shader's inputs
                              // *after* PSSetShader returns. Use this only if you want to
                              // run "between draws", not "in the same draw context as the
                              // matched shader".
    BeforeDrawForMatchedDef, // fires inside MyDraw* hooks, after the engine has set up
                              // the full pipeline for the upcoming draw. State is fresh —
                              // OM RTV / PS SRVs reflect what the matched shader will see.
                              // PREFER THIS for tonemap-piggyback patterns like SSRTGI
                              // composite that need to read/write surfaces the matched
                              // shader is about to consume.
    AtPresent,
    // Fires immediately after DrawWorld::DeferredLightsImpl returns, i.e. once
    // all deferred lighting has resolved but before anything transparent is
    // drawn. A pass that multiplies into the scene here darkens only lit
    // opaque surfaces; particles, smoke and glass then blend over the result
    // instead of being darkened by it.
    AfterDeferredLights
};
enum class BlendMode : uint8_t {
    Opaque,        // dst = src                               (writeMask = 0x0F)
    Additive,      // dst = src + dst                         (HDR add: e.g. SSRTGI composite)
    PremulAlpha,   // dst = src.rgb + dst.rgb * (1 - src.a)   (src is premultiplied)
    Multiply,      // dst = src.rgb * dst.rgb                 (e.g. SSAO darkening)
};

enum class InputKind : uint8_t {
    None,
    Depth,                // g_depthSRV (main scene depth)
    CurrentRTV,           // snapshot SRV of OMGetRenderTargets()[0] at fire time
    CurrentPSRV,          // snapshot SRV of PS tN at fire time
    Resource,             // a customResource (by name)
    GBufferRT,            // g_rendererData->renderTargets[N].srView (explicit index)
    GBufferNormal,        // renderTargets[NORMAL_BUFFER_INDEX].srView   (kGbufferNormal=20)
    GBufferAlbedo,        // renderTargets[kGbufferAlbedo=22].srView
    GBufferMaterial,      // renderTargets[kGbufferMaterial=24].srView
    MotionVectors,        // renderTargets[kMotionVectors=29].srView
    SceneHDR,             // renderTargets[kMain=3].srView (engine HDR scene)
    WaterReflectionCubemap,      // validated completed engine reflection cube
    WaterReflectionCubemapMeta,  // native generation/freshness metadata
    DepthStencil,         // depthStencilTargets[N].srViewDepth (explicit index)
    File,                 // WIC-loaded texture file (path relative to the pass's folder)
};

struct InputBinding {
    int                                     slot = -1;
    InputKind                               kind = InputKind::None;
    std::string                             resourceName;        // for Resource
    int                                     gbufferIndex = -1;   // for GBufferRT
    int                                     depthStencilIndex = -1;  // for DepthStencil
    int                                     sourceSlot = -1;     // for CurrentPSRV
    // For File: lazy-loaded on first fire via the same WIC path replacement
    // shaders use for bindTexture. Released in Pass::Release.
    ReplacementTextureBinding               fileTexture;
};

enum class OutputKind : uint8_t {
    Resource,                  // a customResource RTV (PS) or UAV (CS)
    GBufferRT,                 // g_rendererData->renderTargets[N].rtView (PS only)
    // OMGetRenderTargets()[0] as captured at fire time (PS only). Exists for
    // triggers where the engine's destination surface is not present in
    // renderTargets[] at all — at afterDeferredLights the composite writes an
    // RTV that no table index reaches, so the only way to address it is "the
    // one bound right now". Identity is deliberately unknown; the bind path
    // guards on it being a 2D texture before using it.
    CurrentRTV
};

struct OutputBinding {
    int                                     slot = -1;
    OutputKind                              kind = OutputKind::Resource;
    std::string                             resourceName;        // for Resource
    int                                     gbufferIndex = -1;   // for GBufferRT
    uint32_t                                mipLevel = 0;        // customResource:NAME@mipN
};

struct ThreadGroupDim {
    ScaleMode                               mode = ScaleMode::Absolute;
    uint32_t                                value = 1;           // Absolute: literal; ScreenDiv: divisor
    bool                                    roundUp = false;     // screenceil/N covers partial edge groups
};

struct PassSpec {
    std::string                             name;
    bool                                    active = true;
    // Optional runtime gate. Names a Values.ini-backed bool — when the
    // referenced bool is false the pass is skipped entirely (no fire,
    // no state perturbation), so e.g. flipping `vu_FakeSkinBloomEnabled`
    // off cleanly drops all three blur passes from the per-frame chain.
    // Leading '!' inverts: "activeWhen=!vu_Foo" fires only when vu_Foo
    // is false. Empty string = no gate (always fires).
    std::string                             activeWhen;
    int                                     priority = 0;
    PassType                                type = PassType::Pixel;
    std::filesystem::path                   shaderFile;
    std::string                             entry = "main";
    TriggerKind                             trigger = TriggerKind::None;
    std::string                             triggerUID;          // BeforeShaderUID
    std::string                             triggerHookId;       // BeforeHookId / BeforeDrawForHookId
    // Hint set during INI parsing: pass uses beforeDrawForHook:ID rather
    // than beforeHookId:ID. Read by ResolveHookIdTriggers to pick which
    // index the resolved pass goes into.
    bool                                    atDrawTime = false;
    bool                                    oncePerFrame = true;
    std::vector<InputBinding>               inputs;
    std::vector<OutputBinding>              outputs;             // RTV (PS) or UAV (CS)
    ScaleMode                               viewportMode = ScaleMode::Screen;
    uint32_t                                viewportDiv = 1;
    BlendMode                               blend = BlendMode::Opaque;
    bool                                    clearOnFire = false;
    bool                                    depthTest = false;
    bool                                    log = false;
    // Opt-in asynchronous timestamp queries. Results are polled several
    // frames later with DONOTFLUSH, so diagnostics never stall the renderer.
    bool                                    profileGpu = false;
    std::array<ThreadGroupDim, 3>           threadGroups{};      // CS only
};

class Pass {
public:
    struct GpuTimingSlot {
        REX::W32::ID3D11Query* disjoint = nullptr;
        REX::W32::ID3D11Query* begin = nullptr;
        REX::W32::ID3D11Query* end = nullptr;
        uint32_t submittedFrame = 0;
        bool pending = false;
    };

    PassSpec                                    spec{};
    REX::W32::ID3D11PixelShader*                psShader = nullptr;
    REX::W32::ID3D11ComputeShader*              csShader = nullptr;
    ID3DBlob*                                   compiledBlob = nullptr;
    bool                                        compileTried = false;
    bool                                        compileFailed = false;
    // Optional HLSL hot-reload watcher. Created in DEVELOPMENT mode only.
    // Touching the .hlsl file releases the compiled shader so the next
    // FirePass call re-compiles from disk.
    std::unique_ptr<FileWatcher>                hlslWatcher;
    // Sentinel value so the first frame's gating compare cannot accidentally
    // match the initial currentFrame (= 0) and skip the very first fire.
    std::atomic<uint32_t>                       lastFiredFrame{ UINT32_MAX };
    // Total fires across the lifetime of this pass — used by the debug
    // overlay and the rate-limited fire log.
    std::atomic<uint64_t>                       totalFireCount{ 0 };
    std::array<GpuTimingSlot, 4>                gpuTiming{};
    std::atomic<float>                          gpuLastMs{ 0.0f };
    std::atomic<float>                          gpuAverageMs{ 0.0f };
    std::atomic<uint64_t>                       gpuTimingSamples{ 0 };
    bool                                        gpuTimingUnavailable = false;
    bool                                        gpuTimingFailureLogged = false;
    // Set by FileWatcher when the .hlsl file changes on disk. FirePass
    // observes the flag on the main render thread, drops the compiled
    // shader, and recompiles before dispatching. This avoids the watcher
    // thread mutating D3D11 objects directly (deadlock + use-after-free risk
    // if FirePass was mid-flight).
    std::atomic<bool>                           reloadRequested{ false };
    // Per-pass compile mutex. Held by EnsureCompiled so the background
    // precompile worker and the render-thread FirePass path can't both
    // compile this pass at once. unique_ptr because std::mutex is non-movable.
    std::unique_ptr<std::mutex>                 compileMutex = std::make_unique<std::mutex>();

    struct ActiveWhenTerm {
        void* shaderValue = nullptr;
        bool negate = false;
    };
    // Every cached term must pass. Unknown ids retain the existing fire-open
    // behavior for that term so legacy configurations do not turn off passes.
    std::vector<ActiveWhenTerm>                 activeWhenTerms;
    bool                                        activeWhenChecked = false;

    void Release();
};

struct DrawPassBatch {
    REX::W32::ID3D11PixelShader* originalPS = nullptr;
    ShaderDefinition* matchedDefinition = nullptr;
    std::vector<Pass*> passes;
};

// --- Snapshot SRV cache ---------------------------------------------------

// Maintains an SRV that views the texture currently bound as RTV0 at fire
// time. Engine HDR / scene-color textures are created with BIND_SHADER_RESOURCE
// so we can construct an SRV directly without copying. The cache is keyed by
// texture pointer so we don't leak SRVs when the bound RTV cycles between a
// few stable textures.
class SnapshotSrvCache {
public:
    REX::W32::ID3D11ShaderResourceView* Get(REX::W32::ID3D11Device* device,
                                            REX::W32::ID3D11Texture2D* texture);
    void Release();

private:
    std::unordered_map<REX::W32::ID3D11Texture2D*, REX::W32::ID3D11ShaderResourceView*> entries;
    std::mutex                                                                          mutex;
};

// --- Registry -------------------------------------------------------------

class Registry {
public:
    // Parse a `[customResource:NAME]` or `[customPass:NAME]` block. The opening
    // line has already been consumed by the host parser; `endTag` is the
    // expected `[/customX:NAME]` closer. Reads from `file` until the closer.
    bool ParseSection(const std::string&              sectionName,
                      std::ifstream&                  file,
                      const std::string&              endTag,
                      const std::filesystem::path&    folderPath,
                      const std::string&              folderName);

    static bool IsCustomSection(const std::string& sectionName);

    // Wipe everything; called on hot reload.
    void Reset();

    // Spawn HLSL hot-reload watchers for every pass currently registered.
    // Called once after Shader.ini parsing completes when DEVELOPMENT=true.
    void StartFileWatchers();

    // Mark every pass that has an HLSL file for recompile from disk, exactly
    // as the watcher callback does but without requiring a watcher to exist
    // (they are DEVELOPMENT-only). Sets flags only — the compiled objects are
    // released on the render thread inside FirePassWithSaved. Returns the
    // number of passes marked.
    std::size_t RequestReloadAll();

    // Submit one compile job per registered pass to the background
    // precompile worker. The worker is owned externally (g_precompileWorker)
    // — we just hand it std::function<void()>s. Each job locks the per-pass
    // compileMutex inside EnsureCompiled, so a render-thread FirePass that
    // hits the same pass while it's mid-compile blocks until the worker is
    // done, then short-circuits on the now-populated psShader/csShader.
    void EnqueuePrecompileJobs();

    // Frame hooks ------------------------------------------------------------
    // Called from MyPSSetShader before the engine swap takes effect. Returns
    // true if a pass fired (so caller knows state was disturbed and may need
    // to re-bind injected resources afterwards).
    bool OnBeforeShaderBound(REX::W32::ID3D11DeviceContext* context,
                             REX::W32::ID3D11PixelShader*    originalPS);

    // Called from MyDraw / MyDrawIndexed / MyDrawIndexedInstanced /
    // MyDrawInstanced just BEFORE forwarding to the original, and also from
    // the engine-level HookedBSBatchRendererDraw. The `source` tag is a
    // short label ("d3d11-DrawIndexed", "engine-BSBatch", etc.) recorded in
    // diagnostic logs so we can tell which hook path actually catches a
    // given draw — useful when ENB / ReShade context wrapping makes the
    // D3D11 vtable hooks unreliable. Returns true if a pass fired.
    bool OnBeforeDraw(REX::W32::ID3D11DeviceContext* context, const char* source);
    const DrawPassBatch* ResolveDrawPassBatchForShader(
        REX::W32::ID3D11PixelShader* originalPS,
        std::uint64_t* generation = nullptr);
    bool FireResolvedDrawBatch(REX::W32::ID3D11DeviceContext* context,
                               const DrawPassBatch* batch,
                               std::uint64_t generation,
                               const char* source);
    void InvalidateDrawPassCache();

    // Called from MyPresent. Performs:
    //   - resource allocation for new resources
    //   - copyAt=present operations
    //   - clearOnPresent operations
    //   - any AtPresent passes
    //   - per-frame fired flag reset
    //   - ping-pong swap
    //   - global SRV bind refresh
    void OnFramePresent(REX::W32::ID3D11DeviceContext* context);

    // Called from ShadowUpgrade's DeferredLightsImpl hook, after the original.
    // Fires every AfterDeferredLights pass as one priority-sorted batch.
    void FireAfterDeferredLights(REX::W32::ID3D11DeviceContext* context);

    // Called from BindInjectedPixelShaderResources to keep customResource
    // SRVs bound on their declared slots after engine state changes.
    void BindGlobalResourceSRVs(REX::W32::ID3D11DeviceContext* context, bool pixelStage);
    bool HasGlobalResourceBindings() const noexcept;
    REX::W32::ID3D11ShaderResourceView* GetResourceSRV(const std::string& name);

    // Resolve a hook id (existing [shaderId] section) to its first ShaderUID,
    // used by triggerHookId. Called once after Shader.ini load completes.
    void ResolveHookIdTriggers();

    // Diagnostic for ImGui debug overlay.
    size_t ResourceCount() const;
    size_t PassCount() const;

    // Render the ImGui debug window. Implemented by the ImGui menu module.
    void DrawDebugOverlay();

private:
    bool                                ParseResourceSection(const std::string&,  std::ifstream&, const std::string&);
    bool                                ParsePassSection    (const std::string&,  std::ifstream&,  const std::string&,
                                                            const std::filesystem::path&);
    Resource*                           FindResource(const std::string& name);
    bool                                EnsureCompiled(Pass& pass);
    bool                                EnsurePassResources(Pass& pass);
    // Single-pass entry point: captures D3D state, runs the pass, restores.
    // Used for one-off fires (e.g. AtPresent passes).
    void                                FirePass(REX::W32::ID3D11DeviceContext* context, Pass& pass);
    // Batched entry point: captures D3D state ONCE, fires all matches sorted
    // by priority, restores ONCE. Used when many passes share a trigger
    // (e.g. the SSAO / SSRTGI / fake-skin-bloom chains all firing at
    // beforeDrawForHook:visualTonemap). Eliminates the per-pass
    // save/restore cycle that dominated CPU overhead with large chains.
    // Returns true if any matched pass was active.
    bool                                FireBatch(REX::W32::ID3D11DeviceContext* context,
                                                  std::vector<Pass*>& matches);
    bool                                FireSortedBatch(REX::W32::ID3D11DeviceContext* context,
                                                        const std::vector<Pass*>& matches);
    // Internal: per-pass body shared by FirePass and FireBatch. Assumes
    // the caller has already captured `saved`; reads it for currentRTV
    // snapshot resolution and viewport fallback. Does NOT restore — the
    // caller owns lifecycle.
    bool                                FirePassWithSaved(REX::W32::ID3D11DeviceContext* context,
                                                          Pass& pass,
                                                          SavedState& saved);
    void                                ApplyPingpong();

    // Containers store unique_ptrs so address stability survives rehash.
    std::vector<std::unique_ptr<Resource>>          resources;
    std::vector<std::unique_ptr<Pass>>              passes;
    std::unordered_multimap<std::string, Pass*>          uidIndex;     // BeforeShaderUID
    std::unordered_multimap<std::string, Pass*>          hookIdIndex;  // BeforeHookId / BeforeDrawForHookId pre-resolution
    std::unordered_multimap<ShaderDefinition*, Pass*>    defIndex;     // BeforeMatchedDefinition
    std::unordered_multimap<ShaderDefinition*, Pass*>    drawDefIndex; // BeforeDrawForMatchedDef
    std::unordered_map<REX::W32::ID3D11PixelShader*, std::unique_ptr<DrawPassBatch>> drawBatchCache;
    std::unordered_map<std::string, Resource*>      resourceIndex;  // by name
    SnapshotSrvCache                                snapshotCache;
    std::atomic_bool                                hasDrawTimePasses{ false };
    std::atomic_bool                                hasGlobalResourceBindings{ false };
    std::atomic<std::uint64_t>                      drawPassCacheGeneration{ 1 };
    mutable std::mutex                              mutex;
    uint32_t                                        currentFrame = 0;
};

// Single shared registry. Lifetime: process; reset on Shader.ini hot reload.
extern Registry g_registry;

// Free-function wrapper so hook translation units can fire this trigger without
// including CustomPass.h, which is not standalone (it needs Plugin.h's types
// first). Declared again in ShadowUpgrade.cpp; defined in CustomPass.cpp.
void FireAfterDeferredLightsPasses(REX::W32::ID3D11DeviceContext* context);

// --- Built-in fullscreen VS used by every PS-typed pass -------------------
// Compiled once on first PS pass fire and cached.
REX::W32::ID3D11VertexShader* GetFullscreenTriangleVS(REX::W32::ID3D11Device* device);

}  // namespace CustomPass
