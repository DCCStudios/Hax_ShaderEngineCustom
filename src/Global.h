#pragma once
#include <PCH.h>
#include <Plugin.h>

// Global logger pointer
extern std::shared_ptr<spdlog::logger> gLog;

// Global data handler
extern RE::TESDataHandler* g_dataHandle;
// Global messaging interface
extern const F4SE::MessagingInterface* g_messaging;
// Global task interface for scheduling tasks on the main thread
extern const F4SE::TaskInterface* g_taskInterface;
// Global imgui state flag
extern bool g_imguiInitialized;

// Global module name
extern std::string g_moduleName;
// Global ini file content
extern const char* defaultIni;
// Flash replacement shader HLSL file path
extern const char* flashPixelShaderHLSL;
// Global compiled flash shader
extern REX::W32::ID3D11PixelShader* g_flashPixelShader;
// Global plugin path
extern std::filesystem::path g_pluginPath;
// Global debug flag
extern bool DEBUGGING;
// Master runtime kill switch for shader replacements and custom passes.
extern bool SHADERENGINE_EFFECTS_ON;
// Enables the validated OG deferred-light scope and directional-cascade
// operand patch. Legacy Shader.ini rules may also opt into this gate.
extern bool SHADOW_UPGRADE_ON;
// End distance of the highest-detail directional-shadow cascade. This is
// applied only with SHADOW_UPGRADE_ON and requires a restart.
extern float DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE;
// Custom buffer update flag
extern bool CUSTOMBUFFER_ON;
// Experimental directional shadow-map static-depth cache. Off by default.
// When enabled, it caches precombine-only depth per live directional split
// and overlays dynamic/unknown records until the split cache key changes.
extern bool SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON;
extern bool COMMAND_BUFFER_REPLAY_DEDUPE_SRV;
// Custom resource view slot in shader
extern UINT CUSTOMBUFFER_SLOT;
extern UINT DRAWTAG_SLOT;
// Packed shader settings resource view slots
extern UINT MODULAR_FLOATS_SLOT;
extern UINT MODULAR_INTS_SLOT;
extern UINT MODULAR_BOOLS_SLOT;
// Shader settings menu flag
extern bool SHADERSETTINGS_ON;
// Shader settings menu hotkey (default END key)
extern UINT SHADERSETTINGS_MENUHOTKEY;
// Settings save hotkey (default HOME key)
extern UINT SHADERSETTINGS_SAVEHOTKEY;
// Settings menu width
extern int SHADERSETTINGS_WIDTH;
// Settings menu height
extern int SHADERSETTINGS_HEIGHT;
// Settings menu opacity (0.0 - 1.0)
extern float SHADERSETTINGS_OPACITY;
// Global development flag
extern bool DEVELOPMENT;
// Dev GUI flag
extern bool DEVGUI_ON;
// Dev GUI Width
extern int DEVGUI_WIDTH;
// Dev GUI Height
extern int DEVGUI_HEIGHT;
// Dev GUI Opacity
extern float DEVGUI_OPACITY;
// Global shader settings
extern GlobalShaderSettings g_shaderSettings;
// Shader definitions from INI
extern ShaderDefDB g_shaderDefinitions;
// Shader values from INI
extern std::vector<ShaderValue> g_shaderValues;
// Global original shader database
extern ShaderDB g_ShaderDB;
// Global shader include path
extern std::filesystem::path g_commonShaderHeaderPath;
// Global custom buffer data structure instance for updating CB13
extern GFXBoosterAccessData g_customBufferData;
extern DrawTagData g_drawTagData;
extern std::vector<RaceGroupFormRef> g_raceGroupFormRefs;
extern std::unordered_map<std::uint32_t, std::uint32_t> g_raceGroupMaskByRaceFormID;
extern std::shared_mutex g_raceGroupLock;
extern bool g_raceGroupsResolved;
// Index into g_rendererData->renderTargets[] that the engine uses for the
// G-buffer normal target. Bethesda's deferred renderer doesn't expose a
// canonical name, and the index is not 100% stable across runtime versions,
// so it's exposed as a config knob in ShaderEngine.ini. -1 disables the
// `gbufferNormal` built-in (custom passes that ask for it will get nullptr
// SRV and the consuming shader is expected to fall back).
extern int NORMAL_BUFFER_INDEX;
// Renderer singleton handle (owned by Plugin.cpp). Exposed here so the
// CustomPass module can access device/context/render targets.
extern RE::BSGraphics::RendererData* g_rendererData;
// Main scene depth SRV bound by BindInjectedPixelShaderResources. Exposed so
// custom passes can wire it as an input without re-fetching every fire.
extern REX::W32::ID3D11ShaderResourceView* g_depthSRV;
// Set during replacement-shader compilation/creation to suppress recursive
// shader-creation hooks. Custom passes set the same flag while compiling.
// thread_local so the precompile worker doesn't accidentally cause the
// render thread's shader-creation hook to skip analysis on real game
// shaders (or vice versa).
extern thread_local bool g_isCreatingReplacementShader;
// Original engine PS associated with the currently bound replacement. Updated
// by the PSSetShader hook before ShaderEngine substitutes the D3D11 object.
extern std::atomic<REX::W32::ID3D11PixelShader*> g_currentOriginalPixelShader;
// Set while BindInjectedPixelShaderResources is mutating SRV slots, so the
// PSSetShaderResources hook does not re-enter and create infinite recursion.
// thread_local for the same reason as above (also: PSSetShaderResources is
// only ever hit on the render thread, so this is paranoia + symmetry).
extern thread_local bool g_bindingInjectedPixelResources;
// Set while customPass is issuing its own D3D calls. D3D hooks should treat
// those calls as internal pass work, not as engine draws that need replacement
// shader matching or replacement SRV rebinding.
extern thread_local bool g_customPassRendering;
// Global custom resource to pass data to shaders
extern REX::W32::ID3D11Buffer* g_customSRVBuffer;
extern REX::W32::ID3D11ShaderResourceView* g_customSRV;
extern REX::W32::ID3D11Buffer* g_drawTagSRVBuffer;
extern REX::W32::ID3D11ShaderResourceView* g_drawTagSRV;
extern REX::W32::ID3D11Buffer* g_modularFloatsSRVBuffer;
extern REX::W32::ID3D11ShaderResourceView* g_modularFloatsSRV;
extern REX::W32::ID3D11Buffer* g_modularIntsSRVBuffer;
extern REX::W32::ID3D11ShaderResourceView* g_modularIntsSRV;
extern REX::W32::ID3D11Buffer* g_modularBoolsSRVBuffer;
extern REX::W32::ID3D11ShaderResourceView* g_modularBoolsSRV;
// Global flag if an INI reload is queued
extern std::atomic<bool> g_reloadQueued;

// Helper function to convert string to lowercase
inline std::string ToLower(const std::string& str) {
    std::string out = str;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

std::string GetCommonShaderHeaderHLSLTop();
std::string GetCommonShaderHeaderHLSLBottom();
bool SaveShaderEngineConfig(std::string* errorMessage = nullptr);

// --- Compiled-shader cache ----------------------------------------------
//
// D3DCompile is the slow part of the compile pipeline; CreatePixelShader /
// CreateVertexShader / CreateComputeShader on a pre-compiled blob is
// microseconds. We persist the compiled bytecode to disk keyed on a hash of
// every input that affects codegen — fully assembled HLSL source, target
// profile, entry-point name, compile flags, plus the contents of the common
// include directory — and reuse it on subsequent runs whenever the hash
// matches. If any input changes the hash changes naturally, so edits never
// silently run stale code.
namespace ShaderCache {
    // Bump whenever the on-disk file format itself changes (e.g. header
    // layout). Rolling the assembled-source contents alone does not need a
    // version bump — it's already part of the cache key.
    constexpr uint32_t kFileFormatVersion = 1;

    struct CompileInputs {
        std::string_view assembledSource;  // final source after all define injection
        std::string_view profile;          // "ps_5_0", "vs_5_0", "cs_5_0", ...
        std::string_view entry;            // entry-point function name
        uint32_t         flags;            // D3DCOMPILE_* flags actually passed
    };

    // Stable hex string suitable for use as a filename. Hash covers `inputs`
    // plus every regular file in g_commonShaderHeaderPath (filename +
    // contents, sorted), so any include change invalidates dependent caches.
    std::string ComputeKey(const CompileInputs& inputs);

    // Try to load a previously cached blob for `key`. Allocates a new
    // ID3DBlob via D3DCreateBlob on success; caller owns the reference.
    bool TryLoad(const std::string& key, ID3DBlob** outBlob);

    // Persist `blob` to the cache directory under `key`. Best-effort: write
    // failures are logged but never propagated, since a missing cache just
    // means the next run pays the compile cost again.
    void Store(const std::string& key, ID3DBlob* blob);

    // Drop the memoized include-dir hash and include-file-contents cache.
    // Call after Shader.ini reload (and any other point where the on-disk
    // include set may have changed) so the next ComputeKey / D3DCompile
    // re-reads from disk. Cheap; safe to call from any thread.
    void InvalidateIncludeMemo();
}

// --- Classes ---

// HLSL file watcher class to monitor shader files for changes and trigger recompile
//
// The watcher only flips a flag when a change is seen on disk. The actual
// D3D11 shader Release / replacement-cache eviction is done by the render
// thread (MyPSSetShader / MyVSSetShader) via ConsumeReloadRequest before it
// next calls CompileShader_Internal for the affected definition. This mirrors
// the CustomPass::FileWatcher design and avoids cross-thread Release on
// shader objects that the immediate context may currently have bound — which
// previously caused hard freezes on Shader.ini hot reload.
class HlslFileWatcher {
private:
    std::filesystem::path filePath;
    std::filesystem::file_time_type lastWriteTime;
    std::atomic<bool> reloadRequested{ false };
    std::atomic<bool> running{false};
    std::thread watcherThread;
    // Used so Stop() can interrupt the worker's 1s poll immediately instead
    // of waiting up to a full second for the sleep to expire. Reload tears
    // down every watcher sequentially, so the cumulative blocking time on a
    // Shader.ini save scaled with watcher count (10s+ stalls were typical).
    std::mutex stopMutex;
    std::condition_variable stopCv;
public:
    HlslFileWatcher(std::filesystem::path path)
        : filePath(std::move(path)) {
        if (std::filesystem::exists(filePath)) {
            lastWriteTime = std::filesystem::last_write_time(filePath);
        }
    }
    ~HlslFileWatcher() {
        Stop();
    }
    void Start() {
        running = true;
        watcherThread = std::thread([this]() {
            std::unique_lock lock(stopMutex);
            while (running) {
                lock.unlock();
                Check();
                lock.lock();
                stopCv.wait_for(lock, std::chrono::seconds(1), [this]{ return !running; });
            }
        });
    }
    void Stop() {
        {
            std::lock_guard lock(stopMutex);
            running = false;
        }
        stopCv.notify_all();
        if (watcherThread.joinable()) {
            watcherThread.join();
        }
    }
    // Returns true exactly once per disk-change event; the render thread uses
    // this to know when to drop compiled state before re-compiling.
    bool ConsumeReloadRequest() {
        return reloadRequested.exchange(false, std::memory_order_acq_rel);
    }
    void RequestReload() {
        reloadRequested.store(true, std::memory_order_release);
    }
    void Check() {
        try {
            if (!std::filesystem::exists(filePath)) return;
            auto currentTime = std::filesystem::last_write_time(filePath);
            if (currentTime != lastWriteTime) {
                lastWriteTime = currentTime;
                reloadRequested.store(true, std::memory_order_release);
                REX::INFO("HlslFileWatcher: Shader file '{}' changed, marked for reload", filePath.string());
            }
        } catch (...) {
            // Ignore errors during check
        }
    }
};
// Shader.ini file watcher class to monitor shader files for changes and trigger recompile
class ShaderIniFileWatcher {
private:
    std::filesystem::path filePath;
    std::filesystem::file_time_type lastWriteTime;
    std::string folderName;
    std::atomic<bool> running{false};
    std::thread watcherThread;
    std::mutex stopMutex;
    std::condition_variable stopCv;
public:
    ShaderIniFileWatcher(std::filesystem::path path, std::string folder)
        : filePath(std::move(path)), folderName(std::move(folder)) {
        if (std::filesystem::exists(filePath)) {
            lastWriteTime = std::filesystem::last_write_time(filePath);
        }
    }
    ~ShaderIniFileWatcher() {
        Stop();
    }
    void Start() {
        running = true;
        watcherThread = std::thread([this]() {
            std::unique_lock lock(stopMutex);
            while (running) {
                lock.unlock();
                Check();
                lock.lock();
                stopCv.wait_for(lock, std::chrono::seconds(1), [this]{ return !running; });
            }
        });
    }
    void Stop() {
        {
            std::lock_guard lock(stopMutex);
            running = false;
        }
        stopCv.notify_all();
        if (watcherThread.joinable()) {
            watcherThread.join();
        }
    }
    void Check() {
        try {
            if (!std::filesystem::exists(filePath)) return;
            auto currentTime = std::filesystem::last_write_time(filePath);
            if (currentTime != lastWriteTime) {
                lastWriteTime = currentTime;
                OnFileChanged();
            }
        } catch (...) {
            // Ignore errors during check
        }
    }
private:
    void OnFileChanged() {
        if (g_reloadQueued.exchange(true)) {
            // Already queued, skip
            return;
        }
        if (DEBUGGING)
            REX::INFO("ShaderIniFileWatcher: Detected change in '{}', queuing reload...", filePath.string());
        // Queue the reload on the game main thread
        if (g_taskInterface) {
            g_taskInterface->AddTask([]() {
                // Wrap so an exception during reload doesn't permanently strand
                // g_reloadQueued at true (which would silently disable all
                // future hot reloads).
                try {
                    // Unlock the UI locked shader list to prevent crashes if definitions do not exist anymore
                    UIUnlockShaderList_Internal();
                    ReloadAllShaderDefinitions_Internal();
                } catch (const std::exception& e) {
                    REX::WARN("ShaderIniFileWatcher: Reload task threw: {}", e.what());
                } catch (...) {
                    REX::WARN("ShaderIniFileWatcher: Reload task threw an unknown exception");
                }
                g_reloadQueued = false;  // Reset after reload completes
            });
        } else {
            REX::WARN("ShaderIniFileWatcher: Task interface not available, cannot reload");
            g_reloadQueued = false;  // Avoid stranding the flag if no task IF.
        }
    }
};

// Custom include handler for D3DCompile to resolve #include directives relative to the plugin directory.
// Body lives in Global.cpp so it can share the memoized include-content cache that backs the
// shader-cache include-dir hash.
class ShaderIncludeHandler : public ID3DInclude {
public:
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes) override;
    HRESULT __stdcall Close(LPCVOID pData) override;
};

// Polling watcher on the common include directory. On any mtime change to
// any regular file under g_commonShaderHeaderPath we invalidate the
// ShaderCache include memo so the next compile re-reads everything fresh,
// and bump every active HlslFileWatcher's reloadRequested flag so each
// matched shader recompiles on its next bind. Without this, editing only an
// include silently leaves the running game on stale bytecode AND poisons
// the on-disk cache with the next save of any shader that uses the include.
class IncludeDirWatcher {
public:
    explicit IncludeDirWatcher(std::filesystem::path dir);
    ~IncludeDirWatcher();
    void Start();
    void Stop();

private:
    void Check();
    std::filesystem::path                                              dir;
    // Snapshot of (filename → last_write_time) seen on the previous tick.
    std::unordered_map<std::string, std::filesystem::file_time_type>   snapshot;
    std::atomic<bool>                                                  running{ false };
    std::thread                                                        worker;
    std::mutex                                                         stopMutex;
    std::condition_variable                                            stopCv;
};

extern std::unique_ptr<IncludeDirWatcher> g_includeDirWatcher;

// --- Background precompile worker ---------------------------------------
//
// Compiles replacement shaders and customPass shaders on a background thread
// so the render thread doesn't pay D3DCompile cost on first bind. Each job
// is a std::function<void()> — the caller wraps either CompileShader_Internal
// or CustomPass::Registry::PrecompilePass in a lambda. Per-def / per-pass
// compile mutexes inside those functions make it safe for the render thread
// to compile concurrently (loser observes the winner's compiled state and
// short-circuits).
//
// Lifetime: full Stop()/restart on every Shader.ini reload (around the
// def-deletion window). Stop joins the worker thread, so by the time main
// thread proceeds to delete defs/passes, no in-flight job can dereference
// them.
class PrecompileWorker {
public:
    using Job = std::function<void()>;

    PrecompileWorker() = default;
    ~PrecompileWorker() { Stop(); }
    void Start();
    void Stop();
    // Queue a single job. `name` is logged at the moment the worker actually
    // pops and starts running it — useful for observing what's blocking
    // startup or hot reload. Safe to call from any thread.
    void Enqueue(std::string name, Job job);

private:
    struct NamedJob {
        std::string name;
        Job         job;
    };
    std::deque<NamedJob>      jobs;
    std::mutex                mutex;
    std::condition_variable   cv;
    std::atomic<bool>         running{ false };
    std::thread               worker;
};

extern std::unique_ptr<PrecompileWorker> g_precompileWorker;
