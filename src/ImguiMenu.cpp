#include <Global.h>
#include <ImguiMenu.h>
#include <CustomPass.h>
#include <PhaseTelemetry.h>
#include <ShadowTelemetry.h>

namespace
{
RECT g_windowRect{};
}
// UI: shader-list lock snapshot (when checked we show a frozen list)
static bool g_shaderListLocked = false;
static std::vector<void*> g_lockedShaderKeys; // snapshot of map keys (original shader pointer)
// UI: show/hide replaced shaders in the list
static bool g_showReplaced = true; // default disabled
// UI: show/hide settings menu
static bool g_showSettings = false; // default disabled
static bool g_windowActive = true;
static bool g_shaderSettingsSaveModalRequested = false;
static bool g_shaderSettingsSaveSucceeded = false;
static std::string g_shaderSettingsSaveMessage;

#if SHADERENGINE_ENABLE_PHASE_TELEMETRY
static void ApplyPhaseTelemetrySetting(bool enabled)
{
    const auto mode = PhaseTelemetry::g_mode.load(std::memory_order_relaxed);
    const bool wasEnabled = mode == PhaseTelemetry::Mode::On;
    if (wasEnabled == enabled) {
        return;
    }

    PhaseTelemetry::g_mode.store(
        enabled ? PhaseTelemetry::Mode::On : PhaseTelemetry::Mode::Off,
        std::memory_order_relaxed);
    if (enabled) {
        PhaseTelemetry::RequireHooks();
        PhaseTelemetry::Initialize();
    }
    REX::INFO("ShaderEngine Settings: PHASE_TELEMETRY_MODE set to {}", enabled ? "on" : "off");
}
#endif

#if SHADERENGINE_ENABLE_SHADOW_TELEMETRY
static void ApplyShadowTelemetrySetting(bool enabled)
{
    const auto mode = ShadowTelemetry::g_mode.load(std::memory_order_relaxed);
    const bool wasEnabled = mode == ShadowTelemetry::Mode::On;
    if (wasEnabled == enabled) {
        return;
    }

    ShadowTelemetry::g_mode.store(
        enabled ? ShadowTelemetry::Mode::On : ShadowTelemetry::Mode::Off,
        std::memory_order_relaxed);
    if (enabled) {
        ShadowTelemetry::Initialize();
    }
    REX::INFO("ShaderEngine Settings: SHADOW_TELEMETRY_MODE set to {}", enabled ? "on" : "off");
}
#endif

static void ApplyShadowCacheDirectionalMapSlot1Setting(bool enabled)
{
    if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON == enabled) {
        return;
    }

    SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON = enabled;
    ShadowTelemetry::ResetShadowCacheState();
    if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON) {
        InstallDrawTaggingHooks_Internal();
        ShadowTelemetry::Initialize();
    }
    REX::INFO("ShaderEngine Settings: SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON set to {}", SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON);
}

static void ApplyCommandBufferReplayDedupeSetting(bool enabled)
{
    if (COMMAND_BUFFER_REPLAY_DEDUPE_SRV == enabled) {
        return;
    }

    COMMAND_BUFFER_REPLAY_DEDUPE_SRV = enabled;
    REX::INFO("ShaderEngine Settings: COMMAND_BUFFER_REPLAY_DEDUPE_SRV set to {}", COMMAND_BUFFER_REPLAY_DEDUPE_SRV);
}

static void ApplyShaderEngineEffectsSetting(bool enabled)
{
    if (SHADERENGINE_EFFECTS_ON == enabled) {
        return;
    }

    SHADERENGINE_EFFECTS_ON = enabled;
    REX::INFO("ShaderEngine Settings: SHADERENGINE_EFFECTS_ON set to {}", SHADERENGINE_EFFECTS_ON);
}

static void SaveShaderSettingsWithFeedback()
{
    std::string shaderError;
    std::string configError;
    const bool shaderSaved = g_shaderSettings.SaveSettings(&shaderError);
    const bool configSaved = SaveShaderEngineConfig(&configError);
    g_shaderSettingsSaveSucceeded = shaderSaved && configSaved;
    if (g_shaderSettingsSaveSucceeded) {
        g_shaderSettingsSaveMessage = "Shader settings saved successfully.";
    } else {
        g_shaderSettingsSaveMessage = "Failed to save settings:";
        if (!shaderSaved) {
            g_shaderSettingsSaveMessage += "\nShader values: " + shaderError;
        }
        if (!configSaved) {
            g_shaderSettingsSaveMessage += "\nShaderEngine.ini: " + configError;
        }
    }
    g_shaderSettingsSaveModalRequested = true;
}

// Manual HLSL hot reload. The file watchers that normally drive recompiles
// only exist when DEVELOPMENT=true, so this is the shipped-build path for
// picking up shader edits without a restart: flush the include memo (so the
// on-disk shader cache re-keys against the current include contents), then
// flag every replacement definition and custom pass. Nothing is released
// here - the render thread drops the compiled objects on the next bind/fire,
// since it is the only thread that may touch objects the immediate context
// might currently have bound.
static void ReloadShadersWithFeedback()
{
    ShaderCache::InvalidateIncludeMemo();
    std::size_t definitionCount = 0;
    {
        std::shared_lock lock(g_shaderDefinitions.mutex);
        for (auto* def : g_shaderDefinitions.definitions) {
            if (def && def->active && !def->shaderFile.empty()) {
                ++definitionCount;
            }
        }
    }
    g_manualShaderReloadGeneration.fetch_add(1, std::memory_order_acq_rel);
    const std::size_t passCount = CustomPass::g_registry.RequestReloadAll();

    REX::INFO(
        "ReloadShadersWithFeedback: manual reload requested, generation {} - {} replacement shader(s), {} custom pass(es)",
        g_manualShaderReloadGeneration.load(std::memory_order_relaxed),
        definitionCount,
        passCount);

    g_shaderSettingsSaveSucceeded = true;
    g_shaderSettingsSaveMessage = std::format(
        "Marked {} replacement shader(s) and {} custom pass(es) for reload.\n"
        "Each one recompiles from disk the next time it is used, so edits "
        "appear over the next frame or two.\n"
        "Adding or removing Values.ini settings still needs a game restart.",
        definitionCount,
        passCount);
    g_shaderSettingsSaveModalRequested = true;
}

// UI: Compiler neon flash shader pointer
REX::W32::ID3D11PixelShader* g_flashPixelShader = nullptr;
// UI: Imgui WndProc hook variables
WNDPROC g_originalWndProc = nullptr;
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool IsAltTabSystemKey(UINT msg, WPARAM wParam)
{
    return (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) &&
        (wParam == VK_TAB || wParam == VK_MENU || wParam == VK_LMENU || wParam == VK_RMENU);
}

LRESULT CALLBACK ImGuiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const auto callOriginal = [&]() -> LRESULT {
        return CallWindowProc(g_originalWndProc, hwnd, msg, wParam, lParam);
    };

    switch (msg) {
        case WM_KEYDOWN: {
            // Detect if the key is being held down (using the repeat count bit)
            bool isPressed = (lParam & 0x40000000) == 0x0;
            if (isPressed) {
                // Compare with the stored hotkey
                if (wParam == SHADERSETTINGS_MENUHOTKEY) {
                    g_showSettings = !g_showSettings;
                    ::ShowCursor(g_showSettings);
                }
                else if (wParam == SHADERSETTINGS_SAVEHOTKEY && g_showSettings) {
                    SaveShaderSettingsWithFeedback();
                }
            }
            break;
        }
        case WM_ACTIVATEAPP:
            g_windowActive = (wParam != FALSE);
            if (!g_windowActive) {
                ::ClipCursor(nullptr);
            }
            break;
        case WM_ACTIVATE:
            g_windowActive = (LOWORD(wParam) != WA_INACTIVE);
            if (!g_windowActive) {
                ::ClipCursor(nullptr);
            }
            break;
        case WM_KILLFOCUS:
            g_windowActive = false;
            ::ClipCursor(nullptr);
            break;
        case WM_SETFOCUS:
            g_windowActive = true;
            break;
    }

    if (g_imguiInitialized && g_showSettings){
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

        switch (msg) {
            case WM_ACTIVATE:
            case WM_ACTIVATEAPP:
            case WM_KILLFOCUS:
            case WM_SETFOCUS:
            case WM_SIZE:
            case WM_SYSCOMMAND:
                return callOriginal();
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
                return IsAltTabSystemKey(msg, wParam) ? callOriginal() : 0;
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_CHAR:
            case WM_DEADCHAR:
            case WM_SYSCHAR:
            case WM_SYSDEADCHAR:
            case WM_INPUT:
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
                return 0;
        }

        return callOriginal();
    }
    return callOriginal();
}

bool UIInitialize(HWND hwnd)
{
    REX::INFO("About to initialize ImGui...");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    REX::INFO("HWND: {}", (void*)hwnd);
    if (!hwnd) {
        REX::WARN("Failed to get game window handle");
        return false;
    }
    ::GetWindowRect(hwnd, &g_windowRect);
    g_originalWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)ImGuiWndProc);

    bool endDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
    io.AddKeyEvent(ImGuiKey_End, endDown);
    bool homeDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
    io.AddKeyEvent(ImGuiKey_Home, homeDown);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(
        reinterpret_cast<ID3D11Device*>(g_rendererData->device),
        reinterpret_cast<ID3D11DeviceContext*>(g_rendererData->context)
    );

    if (flashPixelShaderHLSL) {
        ID3DBlob* blob = nullptr;
        ID3DBlob* err  = nullptr;
        HRESULT hr = D3DCompile(
            flashPixelShaderHLSL,
            strlen(flashPixelShaderHLSL),
            "flash_ps",
            nullptr,
            nullptr,
            "main", "ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &blob,
            &err
        );
        if (!REX::W32::SUCCESS(hr)) {
            if (err)
                REX::WARN("Flash shader compile error: {}", static_cast<const char*>(err->GetBufferPointer())); err->Release();
        }
        if (blob) {
            g_isCreatingReplacementShader = true;
            hr = g_rendererData->device->CreatePixelShader(
                blob->GetBufferPointer(),
                blob->GetBufferSize(),
                nullptr,
                &g_flashPixelShader
            );
            g_isCreatingReplacementShader = false;
            blob->Release();
            if (!REX::W32::SUCCESS(hr))
                REX::WARN("CreatePixelShader failed for flash shader with HRESULT: 0x{:08X}", hr);
        }
    }

    REX::INFO("DX11 ImGui initialized");
    g_imguiInitialized = true;
    return true;
}

bool UIIsMenuOpen()
{
    return g_showSettings;
}

bool UIIsWindowActive()
{
    return g_windowActive;
}

const RECT* UIGetWindowRect()
{
    return &g_windowRect;
}

void UIRenderFrame()
{
    if (g_imguiInitialized) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }
    if (g_imguiInitialized && SHADERSETTINGS_ON && g_showSettings) {
        UIDrawShaderSettingsOverlay();
    }
    if (g_imguiInitialized && DEVGUI_ON && g_showSettings) {
        UIDrawShaderDebugOverlay();
        UIDrawCustomBufferMonitorOverlay();
        CustomPass::g_registry.DrawDebugOverlay();
    }
    if (g_imguiInitialized) {
        ImGui::Render();
        auto* context = g_rendererData ? g_rendererData->context : nullptr;
        REX::W32::ID3D11RenderTargetView* oldRTV = nullptr;
        REX::W32::ID3D11DepthStencilView* oldDSV = nullptr;
        if (context) {
            context->OMGetRenderTargets(1, &oldRTV, &oldDSV);
            auto* backBufferRTV = g_rendererData->renderWindow[0].swapChainRenderTarget.rtView;
            if (backBufferRTV) {
                context->OMSetRenderTargets(1, &backBufferRTV, nullptr);
            }
        }
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (context) {
            context->OMSetRenderTargets(1, &oldRTV, oldDSV);
            if (oldRTV) oldRTV->Release();
            if (oldDSV) oldDSV->Release();
        }
    }
}
void UIDrawShaderSettingsOverlay() {
    // Position window in top-right corner
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - static_cast<float>(SHADERSETTINGS_WIDTH) - 10.0f, 10.0f), ImGuiCond_FirstUseEver);
    // Set width/height
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(SHADERSETTINGS_WIDTH), static_cast<float>(SHADERSETTINGS_HEIGHT)), ImGuiCond_FirstUseEver);
    // Make background semi-transparent
    ImGui::SetNextWindowBgAlpha(SHADERSETTINGS_OPACITY);
    // Create the Window
    ImGui::Begin("ShaderEngine Settings");
    if (ImGui::Button("Reset defaults")) {
        for (auto* sValue : g_shaderSettings.GetGlobalShaderValues()) {
            if (sValue) sValue->ResetToDefault();
        }
        for (auto* sValue : g_shaderSettings.GetLocalShaderValues()) {
            if (sValue) sValue->ResetToDefault();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save settings")) {
        SaveShaderSettingsWithFeedback();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload shaders")) {
        ReloadShadersWithFeedback();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Recompile every replacement shader and custom pass from disk.\n"
            "Picks up .hlsl and include edits without restarting the game.\n"
            "Values.ini settings added or removed still need a restart.");
    }
    ImGui::Separator();

    bool shaderEngineEffectsOn = SHADERENGINE_EFFECTS_ON;
    if (ImGui::Checkbox("Enable shader replacements and custom passes", &shaderEngineEffectsOn)) {
        ApplyShaderEngineEffectsSetting(shaderEngineEffectsOn);
    }
    ImGui::Separator();

#if SHADERENGINE_ENABLE_PHASE_TELEMETRY
    bool phaseTelemetryOn =
        PhaseTelemetry::g_mode.load(std::memory_order_relaxed) == PhaseTelemetry::Mode::On;
    if (ImGui::Checkbox("Phase telemetry", &phaseTelemetryOn)) {
        ApplyPhaseTelemetrySetting(phaseTelemetryOn);
    }
#endif
#if SHADERENGINE_ENABLE_SHADOW_TELEMETRY
    bool shadowTelemetryOn =
        ShadowTelemetry::g_mode.load(std::memory_order_relaxed) == ShadowTelemetry::Mode::On;
    if (ImGui::Checkbox("Shadow telemetry", &shadowTelemetryOn)) {
        ApplyShadowTelemetrySetting(shadowTelemetryOn);
    }
#endif
    bool shadowCacheDirectionalMapSlot1On = SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON;
    if (ImGui::Checkbox("Cache directional shadow splits", &shadowCacheDirectionalMapSlot1On)) {
        ApplyShadowCacheDirectionalMapSlot1Setting(shadowCacheDirectionalMapSlot1On);
    }
    bool commandBufferReplayDedupe = COMMAND_BUFFER_REPLAY_DEDUPE_SRV;
    if (ImGui::Checkbox("Dedupe command-buffer replay", &commandBufferReplayDedupe)) {
        ApplyCommandBufferReplayDedupeSetting(commandBufferReplayDedupe);
    }
    ImGui::Separator();

    static std::string editingValueId;
    static bool focusEditInput = false;

    // Render a row for each shader value with appropriate control based on type
    auto renderRow = [&](ShaderValue &sValue) {
        ImGui::PushID(sValue.id.c_str());
        const bool canEditValue = sValue.type == ShaderValue::Type::Int || sValue.type == ShaderValue::Type::Float;
        const bool isEditingValue = canEditValue && editingValueId == sValue.id;

        if (isEditingValue && focusEditInput) {
            ImGui::SetKeyboardFocusHere();
            focusEditInput = false;
        }

        switch (sValue.type) {
            case ShaderValue::Type::Bool:
                if (ImGui::Checkbox(sValue.label.c_str(), &sValue.current.b)) {
                    /* value changed if you need to react */
                }
                break;
            case ShaderValue::Type::Int:
                if (isEditingValue) {
                    if (ImGui::InputInt(sValue.label.c_str(), &sValue.current.i, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        sValue.current.i = std::clamp(sValue.current.i, sValue.min.i, sValue.max.i);
                        editingValueId.clear();
                    } else if (ImGui::IsItemDeactivated()) {
                        sValue.current.i = std::clamp(sValue.current.i, sValue.min.i, sValue.max.i);
                        editingValueId.clear();
                    }
                } else if (ImGui::SliderInt(sValue.label.c_str(), &sValue.current.i, sValue.min.i, sValue.max.i, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                    /* value changed */
                }
                break;
            case ShaderValue::Type::Float:
                if (isEditingValue) {
                    if (ImGui::InputFloat(sValue.label.c_str(), &sValue.current.f, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                        sValue.current.f = std::clamp(sValue.current.f, sValue.min.f, sValue.max.f);
                        editingValueId.clear();
                    } else if (ImGui::IsItemDeactivated()) {
                        sValue.current.f = std::clamp(sValue.current.f, sValue.min.f, sValue.max.f);
                        editingValueId.clear();
                    }
                } else if (ImGui::SliderFloat(sValue.label.c_str(), &sValue.current.f, sValue.min.f, sValue.max.f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
                    /* value changed */
                }
                break;
        }
        if (!sValue.tooltip.empty() && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextUnformatted(sValue.tooltip.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (canEditValue) {
            ImGui::SameLine();
            if (ImGui::SmallButton("E")) {
                editingValueId = sValue.id;
                focusEditInput = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Edit value");
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("R")) {
            sValue.ResetToDefault();
            if (editingValueId == sValue.id) {
                editingValueId.clear();
            }
        }
        ImGui::PopID();
    };
    // Collapsing header for global shader settings
    if (ImGui::CollapsingHeader("Global Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Reset global")) {
            for (auto* sValue : g_shaderSettings.GetGlobalShaderValues()) {
                if (sValue) sValue->ResetToDefault();
            }
        }
        std::map<std::string, std::vector<ShaderValue*>> globalGroups;
        for (auto* sValue : g_shaderSettings.GetGlobalShaderValues()) {
            if (!sValue) continue;
            const std::string groupName = sValue->group.empty() ? "Ungrouped" : sValue->group;
            globalGroups[groupName].push_back(sValue);
        }
        for (auto& kv : globalGroups) {
            const std::string& groupName = kv.first;
            auto& vals = kv.second;
            if (ImGui::CollapsingHeader(groupName.c_str())) {
                ImGui::PushID(groupName.c_str());
                if (ImGui::SmallButton("Reset group")) {
                    for (auto* sValue : vals) {
                        if (sValue) sValue->ResetToDefault();
                    }
                }
                for (auto* sValue : vals) {
                    if (sValue) renderRow(*sValue);
                }
                ImGui::PopID();
            }
        }
    }
    // Collapsing header for active shader definitions and their settings
    if (ImGui::CollapsingHeader("Shader Settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Group local values by explicit Values.ini group, then folder/module.
        std::map<std::string, std::vector<ShaderValue*>> settingsGroups;
        for (auto* sValue : g_shaderSettings.GetLocalShaderValues()) {
            if (!sValue) continue;
            std::string groupName = !sValue->group.empty() ? sValue->group :
                (sValue->folderName.empty() ? sValue->shaderDefinitionId : sValue->folderName);
            settingsGroups[groupName].push_back(sValue);
        }
        // Draw one collapsing header per definition and render children only if open
        for (auto &kv : settingsGroups) {
            const std::string &groupName = kv.first;
            auto &vals = kv.second;
            if (ImGui::CollapsingHeader(groupName.c_str())) {
                ImGui::PushID(groupName.c_str());
                if (ImGui::SmallButton("Reset group")) {
                    for (auto* sValue : vals) {
                        if (sValue) sValue->ResetToDefault();
                    }
                }
                for (auto* sValue : vals) {
                    if (sValue) renderRow(*sValue);
                }
                ImGui::PopID();
            }
        }
    }

    if (g_shaderSettingsSaveModalRequested) {
        ImGui::OpenPopup("Shader settings save");
        g_shaderSettingsSaveModalRequested = false;
    }
    if (ImGui::BeginPopupModal("Shader settings save", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        constexpr float modalContentWidth = 360.0f;
        ImGui::TextColored(
            g_shaderSettingsSaveSucceeded ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "%s",
            g_shaderSettingsSaveSucceeded ? "Success" : "Error");
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + modalContentWidth);
        ImGui::TextWrapped("%s", g_shaderSettingsSaveMessage.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(modalContentWidth, 0.0f));
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

// DEVGUI drawing function for shader debug overlay is called by the Hook Present once per frame
// Should ONLY contain ImGui drawing code!
void UIDrawShaderDebugOverlay() {
    // Position window in top-left corner
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
    // Set width/height
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(DEVGUI_WIDTH), static_cast<float>(DEVGUI_HEIGHT)), ImGuiCond_FirstUseEver);
    // Make background semi-transparent
    ImGui::SetNextWindowBgAlpha(DEVGUI_OPACITY);
    // Create the Window
    ImGui::Begin("ShaderEngine Shader Monitor");
    // Tickboxes for showing replaced shaders and locking the shader list
    ImGui::SameLine(ImGui::GetWindowWidth() - 340);
    if (ImGui::SmallButton("Copy CSV")) {
        // Snapshot the same set of rows the table renders, in the same order,
        // and push them to the clipboard as CSV (definitionId,used,shaderUid).
        // Run before renderRow consumes the recentlyUsed flags so the snapshot
        // reflects what the user sees in the Used column this frame.
        std::string csv =
            "definition,status,used,shaderUid,fxpShaderType,"
            "fxpPixelShaderId\n";
        auto appendRow = [&](const ShaderDBEntry& e) {
            const ShaderDefinition* def = e.GetMatchedDefinition();
            const char* id = def ? def->id.c_str() : "<removed>";
            const bool used = e.IsRecentlyUsed();
            const char* uid = e.shaderUID.empty() ? "<unknown>" : e.shaderUID.c_str();
            const bool hasReplacement = (e.type == ShaderType::Pixel)
                ? (e.GetReplacementPixelShader() != nullptr)
                : (e.GetReplacementVertexShader() != nullptr);
            const bool hasShaderFile = def && !def->shaderFile.empty();
            const bool isBuggy = def && def->buggy;
            const bool featureDisabled =
                def && def->shadowUpgrade && !SHADOW_UPGRADE_ON;
            const char* status = featureDisabled
                ? "DISABLED"
                : (hasReplacement
                       ? "REPLACED"
                       : (isBuggy ? "INVALID"
                                  : (hasShaderFile ? "PENDING" : "MATCHED")));
            csv += id;
            csv += ',';
            csv += status;
            csv += used ? ",YES," : ",NO,";
            csv += uid;
            csv += ',';
            if (e.hasFxpShaderType) {
                csv += std::format("0x{:08X}", e.fxpShaderType);
            }
            csv += ',';
            if (e.hasFxpPixelShaderID) {
                csv += std::format("0x{:08X}", e.fxpPixelShaderID);
            }
            csv += '\n';
        };
        std::shared_lock lock(g_ShaderDB.mutex);
        if (g_shaderListLocked) {
            for (void* key : g_lockedShaderKeys) {
                auto it = g_ShaderDB.entries.find(key);
                if (it == g_ShaderDB.entries.end()) continue;
                if (!g_showReplaced) {
                    bool isReplacedNonFlash =
                        (it->second.type == ShaderType::Pixel && it->second.GetReplacementPixelShader() && it->second.GetReplacementPixelShader() != g_flashPixelShader)
                     || (it->second.type == ShaderType::Vertex && it->second.GetReplacementVertexShader());
                    if (isReplacedNonFlash) continue;
                }
                appendRow(it->second);
            }
        } else {
            for (auto& [ptr, entry] : g_ShaderDB.entries) {
                if (!entry.IsMatched() || !entry.GetMatchedDefinition()) continue;
                if (!g_showReplaced) {
                    bool isReplacedNonFlash =
                        (entry.type == ShaderType::Pixel && entry.GetReplacementPixelShader() && entry.GetReplacementPixelShader() != g_flashPixelShader)
                     || (entry.type == ShaderType::Vertex && entry.GetReplacementVertexShader());
                    if (isReplacedNonFlash) continue;
                }
                appendRow(entry);
            }
        }
        ImGui::SetClipboardText(csv.c_str());
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 240);
    ImGui::Checkbox("Replaced", &g_showReplaced);
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    if (ImGui::Checkbox("Lock list", &g_shaderListLocked)) {
        if (g_shaderListLocked) UILockShaderList_Internal();
        else UIUnlockShaderList_Internal();
    }
    // Collapsing header for active definitions and their matched shaders
    if (ImGui::CollapsingHeader("Active Definitions", ImGuiTreeNodeFlags_DefaultOpen)) {
        constexpr ImGuiTableFlags shaderTableFlags =
            ImGuiTableFlags_Sortable
          | ImGuiTableFlags_BordersInnerV
          | ImGuiTableFlags_RowBg
          | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("shader_columns", 5, shaderTableFlags)) {
            const float charW = ImGui::CalcTextSize("W").x;
            // Sortable columns use stable user IDs so layout/index changes don't break the comparator switch.
            // ID is the default sort, ascending; matches "sort by name, ascending" requirement.
            ImGui::TableSetupColumn("ID",        ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortAscending | ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            ImGui::TableSetupColumn("Status",    ImGuiTableColumnFlags_WidthFixed, charW * 9.0f, 1);
            ImGui::TableSetupColumn("Used",      ImGuiTableColumnFlags_WidthFixed, charW * 5.0f, 2);
            ImGui::TableSetupColumn("ShaderUID", ImGuiTableColumnFlags_WidthFixed, charW * 14.0f, 3);
            ImGui::TableSetupColumn("Action",    ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, charW * 8.0f, 4);
            ImGui::TableHeadersRow();
            // Render a single row
            auto renderRow = [&](ShaderDBEntry& entry) {
                ShaderDefinition* def = entry.GetMatchedDefinition();
                ImGui::TableNextRow();
                // Column 0: ID
                ImGui::TableSetColumnIndex(0);
                const char* id = def ? def->id.c_str() : "<removed>";
                bool hasReplacement = (entry.type == ShaderType::Pixel) ? (entry.GetReplacementPixelShader() != nullptr) : (entry.GetReplacementVertexShader() != nullptr);
                bool hasShaderFile = def && !def->shaderFile.empty();
                bool featureDisabled = def && def->shadowUpgrade && !SHADOW_UPGRADE_ON;
                if (featureDisabled) ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "%s", id);
                else if (hasReplacement) ImGui::TextColored(ImVec4(0,1,0,1), "%s", id);
                else if (def && def->buggy) ImGui::TextColored(ImVec4(1,0.25f,0.25f,1), "%s", id);
                else if (hasShaderFile) ImGui::TextColored(ImVec4(1,0.5f,0,1), "%s", id);
                else ImGui::TextColored(ImVec4(1,1,0,1), "%s", id);
                if (entry.hasFxpPixelShaderID && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "FXP family 0x%08X, permutation 0x%08X",
                        entry.hasFxpShaderType ? entry.fxpShaderType : 0xFFFFFFFFu,
                        entry.fxpPixelShaderID);
                }
                // Column 1: Status
                ImGui::TableSetColumnIndex(1);
                if (featureDisabled) ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "DISABLED");
                else if (hasReplacement) ImGui::TextColored(ImVec4(0,1,0,1), "REPLACED");
                else if (def && def->buggy) ImGui::TextColored(ImVec4(1,0.25f,0.25f,1), "INVALID");
                else if (hasShaderFile) ImGui::TextColored(ImVec4(1,0.5f,0,1), "PENDING");
                else ImGui::TextColored(ImVec4(1,1,0,1), "MATCHED");
                // Column 2: Recently used
                ImGui::TableSetColumnIndex(2);
                bool usedThisFrame = entry.IsRecentlyUsed();
                if (usedThisFrame) {
                    ImGui::TextColored(ImVec4(0,1,1,1), "YES");
                    entry.SetRecentlyUsed(false); // reset for next frame
                } else {
                    ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "NO");
                }
                // Column 3: ShaderUID
                ImGui::TableSetColumnIndex(3);
                const char* shaderUID = entry.shaderUID.empty() ? "<unknown>" : entry.shaderUID.c_str();
                ImGui::Text("%s", shaderUID);
                // Column 4: Actions (Flash)
                ImGui::TableSetColumnIndex(4);
                ImGui::PushID((void*)entry.originalShader); // Use original shader pointer as unique ID to avoid ID collisions in the list
                auto* currentShader = entry.GetReplacementPixelShader();
                if (entry.type == ShaderType::Pixel && (!currentShader || currentShader == g_flashPixelShader)) {
                    ImGui::BeginDisabled(!g_flashPixelShader && currentShader != g_flashPixelShader);
                    if (ImGui::SmallButton(currentShader == g_flashPixelShader ? "Unflash" : "Flash")) {
                        entry.SetReplacementPixelShader(currentShader == g_flashPixelShader ? nullptr : g_flashPixelShader);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::PopID();
            };
            // Build snapshot, sort per current sort spec, then render. Lock held throughout
            // so the underlying entries (referenced by raw pointer in the snapshot) stay alive.
            {
                std::shared_lock lock(g_ShaderDB.mutex);
                std::vector<ShaderDBEntry*> rows;
                if (g_shaderListLocked) {
                    rows.reserve(g_lockedShaderKeys.size());
                    for (void* key : g_lockedShaderKeys) {
                        auto it = g_ShaderDB.entries.find(key);
                        if (it == g_ShaderDB.entries.end()) {
                            // Skip if the entry no longer exists in the ShaderDB
                            continue;
                        }
                        // Filter out replaced shaders if the option is disabled
                        if (!g_showReplaced) {
                            bool isReplacedNonFlash =
                                (it->second.type == ShaderType::Pixel && it->second.GetReplacementPixelShader() && it->second.GetReplacementPixelShader() != g_flashPixelShader)
                            || (it->second.type == ShaderType::Vertex && it->second.GetReplacementVertexShader());
                            if (isReplacedNonFlash)
                                continue;
                        }
                        rows.push_back(&it->second);
                    }
                } else {
                    rows.reserve(g_ShaderDB.entries.size());
                    for (auto& [ptr, entry] : g_ShaderDB.entries) {
                        ShaderDefinition* def = entry.GetMatchedDefinition();
                        if (entry.IsMatched() && entry.IsRecentlyUsed() && def) {
                            // Filter out replaced shaders if the option is disabled
                            if (!g_showReplaced) {
                                bool isReplacedNonFlash =
                                    (entry.type == ShaderType::Pixel && entry.GetReplacementPixelShader() && entry.GetReplacementPixelShader() != g_flashPixelShader)
                                || (entry.type == ShaderType::Vertex && entry.GetReplacementVertexShader());
                                if (isReplacedNonFlash)
                                    continue;
                            }
                            rows.push_back(&entry);
                        }
                    }
                }
                // Case-insensitive string compare for alphabetical sorts on ID and ShaderUID.
                auto ciCompare = [](const char* a, const char* b) -> int {
                    while (*a && *b) {
                        const int ca = std::tolower(static_cast<unsigned char>(*a));
                        const int cb = std::tolower(static_cast<unsigned char>(*b));
                        if (ca != cb) return ca - cb;
                        ++a; ++b;
                    }
                    return static_cast<int>(static_cast<unsigned char>(*a)) - static_cast<int>(static_cast<unsigned char>(*b));
                };
                auto statusRank = [](const ShaderDBEntry* e) -> int {
                    const ShaderDefinition* d = e->GetMatchedDefinition();
                    const bool hasReplacement = (e->type == ShaderType::Pixel)
                        ? (e->GetReplacementPixelShader() != nullptr)
                        : (e->GetReplacementVertexShader() != nullptr);
                    const bool hasShaderFile = d && !d->shaderFile.empty();
                    if (d && d->shadowUpgrade && !SHADOW_UPGRADE_ON) return 0;
                    if (hasReplacement) return 1;
                    if (d && d->buggy) return 2;
                    if (hasShaderFile) return 3;
                    return 4;
                };
                ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
                if (sortSpecs && sortSpecs->SpecsCount > 0) {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                    const bool ascending = spec.SortDirection != ImGuiSortDirection_Descending;
                    std::sort(rows.begin(), rows.end(), [&](ShaderDBEntry* a, ShaderDBEntry* b) {
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                            case 0: { // ID (Name)
                                const ShaderDefinition* da = a->GetMatchedDefinition();
                                const ShaderDefinition* db = b->GetMatchedDefinition();
                                cmp = ciCompare(da ? da->id.c_str() : "<removed>",
                                                db ? db->id.c_str() : "<removed>");
                                break;
                            }
                            case 1: // Status
                                cmp = statusRank(a) - statusRank(b);
                                break;
                            case 2: // Used: NO < YES (false < true) for ascending
                                cmp = (a->IsRecentlyUsed() ? 1 : 0) - (b->IsRecentlyUsed() ? 1 : 0);
                                break;
                            case 3: // ShaderUID
                                cmp = ciCompare(a->shaderUID.empty() ? "<unknown>" : a->shaderUID.c_str(),
                                                b->shaderUID.empty() ? "<unknown>" : b->shaderUID.c_str());
                                break;
                            default:
                                break;
                        }
                        if (cmp == 0) {
                            // Stable tie-breaker keeps row order deterministic across frames
                            return a < b;
                        }
                        return ascending ? (cmp < 0) : (cmp > 0);
                    });
                }
                for (ShaderDBEntry* entry : rows) {
                    renderRow(*entry);
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// DEVGUI drawing function for the custom t# buffer. This reads the CPU-side
// snapshot immediately after UpdateCustomBuffer_Internal has populated it.
void UIDrawCustomBufferMonitorOverlay() {
    static GFXBoosterAccessData previousData{};
    static DrawTagData previousDrawTag{};
    static std::uint64_t previousDrawCalls = 0;
    static bool hasPreviousData = false;

    const GFXBoosterAccessData data = g_customBufferData;
    const DrawTagData drawTag = g_drawTagData;
    const std::uint64_t drawCalls = GetD3DDrawCallsLastFrame_Internal();

    ImGui::SetNextWindowPos(ImVec2(10.0f, 320.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560.0f, 640.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(DEVGUI_OPACITY);
    ImGui::Begin("ShaderEngine Custom Buffer Monitor");

    ImGui::Text("Custom SRV slot: t%u", CUSTOMBUFFER_SLOT);
    ImGui::Text("Draw tag SRV slot: t%u", DRAWTAG_SLOT);
    ImGui::Text("Custom buffer: %p", static_cast<void*>(g_customSRVBuffer));
    ImGui::Text("Custom SRV:    %p", static_cast<void*>(g_customSRV));
    ImGui::Text("DrawTag buffer:%p", static_cast<void*>(g_drawTagSRVBuffer));
    ImGui::Text("DrawTag SRV:   %p", static_cast<void*>(g_drawTagSRV));
    ImGui::Separator();

    // ImGui Columns() doesn't persist user drag-resize across frames; the
    // SetColumnWidth calls every frame would clobber any drag anyway. Tables
    // give proper resizable / reorderable columns.
    constexpr ImGuiTableFlags kCBTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_RowBg     | ImGuiTableFlags_SizingStretchProp;

    auto beginColumns = [](const char* id) -> bool {
        if (!ImGui::BeginTable(id, 3, kCBTableFlags)) return false;
        ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("Delta", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        return true;
    };

    auto endColumns = []() { ImGui::EndTable(); };

    auto renderFloat = [&](const char* label, float value, float previous, float warnDelta = 0.25f) {
        const float delta = hasPreviousData ? value - previous : 0.0f;
        const bool invalid = !std::isfinite(value);
        const bool jump = hasPreviousData && std::fabs(delta) >= warnDelta;
        const ImVec4 valueColor = invalid ? ImVec4(1.0f, 0.1f, 0.1f, 1.0f) : (jump ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
        ImGui::TableNextColumn(); ImGui::TextColored(valueColor, "%.6f", value);
        ImGui::TableNextColumn();
        if (hasPreviousData) {
            ImGui::TextColored(jump ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%+.6f", delta);
        } else {
            ImGui::TextDisabled("-");
        }
    };

    auto renderUInt = [&](const char* label, uint32_t value, uint32_t previous) {
        const bool changed = hasPreviousData && value != previous;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
        ImGui::TableNextColumn(); ImGui::TextColored(changed ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "0x%08X", value);
        ImGui::TableNextColumn();
        if (hasPreviousData) {
            ImGui::Text("%+lld", static_cast<long long>(value) - static_cast<long long>(previous));
        } else {
            ImGui::TextDisabled("-");
        }
    };

    auto renderInt = [&](const char* label, int32_t value, int32_t previous) {
        const bool changed = hasPreviousData && value != previous;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
        ImGui::TableNextColumn(); ImGui::TextColored(changed ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%d", value);
        ImGui::TableNextColumn();
        if (hasPreviousData) {
            ImGui::Text("%+d", value - previous);
        } else {
            ImGui::TextDisabled("-");
        }
    };

    auto renderUInt64Dec = [&](const char* label, std::uint64_t value, std::uint64_t previous) {
        const bool changed = hasPreviousData && value != previous;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
        ImGui::TableNextColumn(); ImGui::TextColored(changed ? ImVec4(1.0f, 0.8f, 0.1f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%llu", static_cast<unsigned long long>(value));
        ImGui::TableNextColumn();
        if (hasPreviousData) {
            ImGui::Text("%+lld", static_cast<long long>(value) - static_cast<long long>(previous));
        } else {
            ImGui::TextDisabled("-");
        }
    };

    auto renderFloat4 = [&](const char* label, const DirectX::XMFLOAT4& value, const DirectX::XMFLOAT4& previous, float warnDelta = 0.25f) {
        renderFloat((std::string(label) + ".x").c_str(), value.x, previous.x, warnDelta);
        renderFloat((std::string(label) + ".y").c_str(), value.y, previous.y, warnDelta);
        renderFloat((std::string(label) + ".z").c_str(), value.z, previous.z, warnDelta);
        renderFloat((std::string(label) + ".w").c_str(), value.w, previous.w, warnDelta);
    };

    if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_frame_columns")) {
            renderFloat("time", data.time, previousData.time, 1.0f);
            renderFloat("delta", data.delta, previousData.delta, 0.05f);
            renderFloat("frame", data.frame, previousData.frame, 2.0f);
            renderUInt64Dec("drawCalls", drawCalls, previousDrawCalls);
            renderFloat("fps", data.fps, previousData.fps, 10.0f);
            renderFloat("random", data.random, previousData.random, 0.9f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Scene State", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_scene_columns")) {
            renderFloat("dayCycle", data.dayCycle, previousData.dayCycle, 0.05f);
            renderFloat("timeOfDay", data.timeOfDay, previousData.timeOfDay, 0.25f);
            renderFloat("weatherTransition", data.weatherTransition, previousData.weatherTransition, 0.05f);
            renderUInt("currentWeatherID", data.currentWeatherID, previousData.currentWeatherID);
            renderUInt("outgoingWeatherID", data.outgoingWeatherID, previousData.outgoingWeatherID);
            renderUInt("currentLocationID", data.currentLocationID, previousData.currentLocationID);
            renderUInt("worldSpaceID", data.worldSpaceID, previousData.worldSpaceID);
            renderUInt("skyMode", data.skyMode, previousData.skyMode);
            renderInt("currentWeatherClass", data.currentWeatherClass, previousData.currentWeatherClass);
            renderInt("outgoingWeatherClass", data.outgoingWeatherClass, previousData.outgoingWeatherClass);
            renderFloat("inInterior", data.inInterior, previousData.inInterior, 0.5f);
            renderFloat("inCombat", data.inCombat, previousData.inCombat, 0.5f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_camera_columns")) {
            renderFloat("camX", data.camX, previousData.camX, 100.0f);
            renderFloat("camY", data.camY, previousData.camY, 100.0f);
            renderFloat("camZ", data.camZ, previousData.camZ, 100.0f);
            renderFloat("viewDirX", data.viewDirX, previousData.viewDirX, 0.25f);
            renderFloat("viewDirY", data.viewDirY, previousData.viewDirY, 0.25f);
            renderFloat("viewDirZ", data.viewDirZ, previousData.viewDirZ, 0.25f);
            renderFloat("vpLeft", data.vpLeft, previousData.vpLeft, 1.0f);
            renderFloat("vpTop", data.vpTop, previousData.vpTop, 1.0f);
            renderFloat("vpWidth", data.vpWidth, previousData.vpWidth, 1.0f);
            renderFloat("vpHeight", data.vpHeight, previousData.vpHeight, 1.0f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_fog_columns")) {
            renderFloat4("g_FogDistances0", data.g_FogDistances0, previousData.g_FogDistances0, 50.0f);
            renderFloat4("g_FogDistances1", data.g_FogDistances1, previousData.g_FogDistances1, 50.0f);
            renderFloat4("g_FogParams", data.g_FogParams, previousData.g_FogParams, 0.25f);
            renderFloat4("g_FogColor", data.g_FogColor, previousData.g_FogColor, 0.05f);
            renderFloat("g_SunR", data.g_SunR, previousData.g_SunR, 0.05f);
            renderFloat("g_SunG", data.g_SunG, previousData.g_SunG, 0.05f);
            renderFloat("g_SunB", data.g_SunB, previousData.g_SunB, 0.05f);
            renderFloat("g_SunDirX", data.g_SunDirX, previousData.g_SunDirX, 0.05f);
            renderFloat("g_SunDirY", data.g_SunDirY, previousData.g_SunDirY, 0.05f);
            renderFloat("g_SunDirZ", data.g_SunDirZ, previousData.g_SunDirZ, 0.05f);
            renderFloat("g_SunValid", data.g_SunValid, previousData.g_SunValid, 0.5f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Matrices")) {
        if (beginColumns("custom_buffer_matrix_columns")) {
            renderFloat4("InvProjRow0", data.g_InvProjRow0, previousData.g_InvProjRow0, 0.05f);
            renderFloat4("InvProjRow1", data.g_InvProjRow1, previousData.g_InvProjRow1, 0.05f);
            renderFloat4("InvProjRow2", data.g_InvProjRow2, previousData.g_InvProjRow2, 0.05f);
            renderFloat4("InvProjRow3", data.g_InvProjRow3, previousData.g_InvProjRow3, 0.05f);
            renderFloat4("InvViewRow0", data.g_InvViewRow0, previousData.g_InvViewRow0, 0.05f);
            renderFloat4("InvViewRow1", data.g_InvViewRow1, previousData.g_InvViewRow1, 0.05f);
            renderFloat4("InvViewRow2", data.g_InvViewRow2, previousData.g_InvViewRow2, 0.05f);
            renderFloat4("InvViewRow3", data.g_InvViewRow3, previousData.g_InvViewRow3, 0.05f);
            renderFloat4("ViewProjRow0", data.g_ViewProjRow0, previousData.g_ViewProjRow0, 0.05f);
            renderFloat4("ViewProjRow1", data.g_ViewProjRow1, previousData.g_ViewProjRow1, 0.05f);
            renderFloat4("ViewProjRow2", data.g_ViewProjRow2, previousData.g_ViewProjRow2, 0.05f);
            renderFloat4("ViewProjRow3", data.g_ViewProjRow3, previousData.g_ViewProjRow3, 0.05f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Player/Input")) {
        if (beginColumns("custom_buffer_player_columns")) {
            renderFloat("resX", data.resX, previousData.resX, 1.0f);
            renderFloat("resY", data.resY, previousData.resY, 1.0f);
            renderFloat("mouseX", data.mouseX, previousData.mouseX, 0.25f);
            renderFloat("mouseY", data.mouseY, previousData.mouseY, 0.25f);
            renderFloat("pHealthPerc", data.pHealthPerc, previousData.pHealthPerc, 0.05f);
            renderFloat("pRadDmg", data.pRadDmg, previousData.pRadDmg, 0.25f);
            renderFloat("windSpeed", data.windSpeed, previousData.windSpeed, 0.25f);
            renderFloat("windAngle", data.windAngle, previousData.windAngle, 0.25f);
            renderFloat("windTurb", data.windTurb, previousData.windTurb, 0.25f);
            endColumns();
        }
    }

    if (ImGui::CollapsingHeader("Draw Tag", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (beginColumns("custom_buffer_drawtag_columns")) {
            renderFloat("materialTag", drawTag.materialTag, previousDrawTag.materialTag, 0.5f);
            renderFloat("isHead",      drawTag.isHead,      previousDrawTag.isHead,      0.5f);
            renderUInt("raceGroupMask", drawTag.raceGroupMask, previousDrawTag.raceGroupMask);
            renderUInt("raceFlags",     drawTag.raceFlags,     previousDrawTag.raceFlags);
            endColumns();
        }
    }

    previousData = data;
    previousDrawTag = drawTag;
    previousDrawCalls = drawCalls;
    hasPreviousData = true;

    ImGui::End();
}
// Lock the current shader list in the UI to prevent it from changing
void UILockShaderList_Internal() {
    std::shared_lock lock(g_ShaderDB.mutex);
    g_lockedShaderKeys.clear();
    g_lockedShaderKeys.reserve(g_ShaderDB.entries.size());
    for (auto& [shaderKey, entry] : g_ShaderDB.entries) {
        ShaderDefinition* def = entry.GetMatchedDefinition();
        // Snapshot **what is currently visible** in the UI
        if (entry.IsMatched() && entry.IsRecentlyUsed() && def) {
            g_lockedShaderKeys.push_back(shaderKey);
        }
    }
}
void UIUnlockShaderList_Internal() {
    g_shaderListLocked = false;
    g_lockedShaderKeys.clear();
}

namespace CustomPass {
namespace {
const char* TriggerName(TriggerKind k) {
    switch (k) {
        case TriggerKind::BeforeShaderUID:         return "beforeShaderUID";
        case TriggerKind::BeforeHookId:            return "beforeHookId(unresolved)";
        case TriggerKind::BeforeMatchedDefinition: return "beforeMatchedDef";
        case TriggerKind::BeforeDrawForMatchedDef: return "beforeDrawForMatchedDef";
        case TriggerKind::AtPresent:               return "atPresent";
        default:                                   return "none";
    }
}
const char* PassTypeName(PassType t) { return (t == PassType::Compute) ? "cs" : "ps"; }
}

void Registry::DrawDebugOverlay() {
    std::lock_guard lk(mutex);
    ImGui::SetNextWindowPos(ImVec2(580.0f, 320.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(DEVGUI_OPACITY);
    ImGui::Begin("ShaderEngine Custom Passes");

    ImGui::Text("Frame: %u    Passes: %zu    Resources: %zu",
                currentFrame, passes.size(), resources.size());
    ImGui::Separator();

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::CollapsingHeader("Passes", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("passes_tbl", 7, kTableFlags)) {
            ImGui::TableSetupColumn("Name",      ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Kind",      ImGuiTableColumnFlags_WidthFixed,  40.0f);
            ImGui::TableSetupColumn("Status",    ImGuiTableColumnFlags_WidthFixed,  60.0f);
            ImGui::TableSetupColumn("GPU ms",    ImGuiTableColumnFlags_WidthFixed,  65.0f);
            ImGui::TableSetupColumn("LastFired", ImGuiTableColumnFlags_WidthFixed,  90.0f);
            ImGui::TableSetupColumn("Trigger",   ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Detail",    ImGuiTableColumnFlags_WidthStretch, 2.5f);
            ImGui::TableHeadersRow();

            for (auto& p : passes) {
                const auto& s = p->spec;
                const uint32_t lf = p->lastFiredFrame.load(std::memory_order_relaxed);
                const uint64_t fires = p->totalFireCount.load(std::memory_order_relaxed);

                ImGui::PushID(p.get());
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(s.name.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Total fires: %llu\nShader: %s",
                        static_cast<unsigned long long>(fires),
                        s.shaderFile.string().c_str());
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(PassTypeName(s.type));

                ImGui::TableSetColumnIndex(2);
                const char* status = !s.active        ? "off"
                                  : p->compileFailed  ? "ERR"
                                  : !p->compiledBlob  ? "..."
                                  :                     "OK";
                ImVec4 col = !s.active        ? ImVec4(0.6f, 0.6f, 0.6f, 1)
                           : p->compileFailed ? ImVec4(0.95f, 0.3f, 0.3f, 1)
                           : !p->compiledBlob ? ImVec4(0.85f, 0.75f, 0.3f, 1)
                           :                    ImVec4(0.4f, 0.95f, 0.4f, 1);
                ImGui::TextColored(col, "%s", status);

                ImGui::TableSetColumnIndex(3);
                const uint64_t timingSamples =
                    p->gpuTimingSamples.load(std::memory_order_relaxed);
                if (!s.profileGpu) {
                    ImGui::TextUnformatted("-");
                } else if (timingSamples == 0) {
                    ImGui::TextUnformatted("...");
                } else {
                    const float averageMs =
                        p->gpuAverageMs.load(std::memory_order_relaxed);
                    const float lastMs =
                        p->gpuLastMs.load(std::memory_order_relaxed);
                    ImGui::Text("%.3f", averageMs);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Asynchronous GPU time\nEMA: %.3f ms\nLast: %.3f ms\nSamples: %llu\nNo flush or render-thread wait",
                            averageMs,
                            lastMs,
                            static_cast<unsigned long long>(timingSamples));
                    }
                }

                ImGui::TableSetColumnIndex(4);
                // The pass fires DURING frame F's draws; OnFramePresent then
                // increments currentFrame to F+1; this overlay renders AFTER
                // the increment. So a pass with lastFired == currentFrame-1
                // ran in the most recent fully-drawn frame, which the user
                // will perceive as "this frame".
                if (lf == UINT32_MAX) {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "never");
                } else {
                    const uint32_t age = currentFrame - lf;
                    if (age <= 1) {
                        ImGui::TextColored(ImVec4(0.4f, 0.95f, 0.4f, 1.0f), "this frame");
                    } else if (age < 10000) {
                        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.4f, 1.0f), "%u frames ago", age);
                    } else {
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "stale");
                    }
                }

                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(TriggerName(s.trigger));

                ImGui::TableSetColumnIndex(6);
                switch (s.trigger) {
                    case TriggerKind::BeforeShaderUID:
                        ImGui::TextUnformatted(s.triggerUID.c_str());
                        break;
                    case TriggerKind::BeforeHookId:
                        // Pre-resolution. ResolveHookIdTriggers will convert
                        // this to BeforeMatchedDefinition before passes fire.
                        ImGui::TextUnformatted(
                            s.triggerHookId.empty() ? "(unresolved)" : s.triggerHookId.c_str());
                        break;
                    case TriggerKind::BeforeMatchedDefinition:
                        ImGui::Text("[%s]", s.triggerHookId.c_str());
                        break;
                    case TriggerKind::BeforeDrawForMatchedDef:
                        ImGui::Text("[%s]@draw", s.triggerHookId.c_str());
                        break;
                    default:
                        ImGui::TextUnformatted("-");
                        break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Resources", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("resources_tbl", 5, kTableFlags)) {
            ImGui::TableSetupColumn("Name",      ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Size",      ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Slot",      ImGuiTableColumnFlags_WidthFixed,  50.0f);
            ImGui::TableSetupColumn("Allocated", ImGuiTableColumnFlags_WidthFixed,  80.0f);
            ImGui::TableSetupColumn("Pingpong",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableHeadersRow();

            for (auto& r : resources) {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r->spec.name.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%ux%u", r->width, r->height);

                ImGui::TableSetColumnIndex(2);
                if (r->spec.srvSlot >= 0) ImGui::Text("t%d", r->spec.srvSlot);
                else                      ImGui::TextUnformatted("-");

                ImGui::TableSetColumnIndex(3);
                const bool ok = r->texture && r->srv;
                ImGui::TextColored(ok ? ImVec4(0.4f, 0.95f, 0.4f, 1.0f) : ImVec4(0.95f, 0.3f, 0.3f, 1.0f),
                                   "%s", ok ? "yes" : "NO");

                ImGui::TableSetColumnIndex(4);
                if (r->pingpongPartner) ImGui::Text("<-> %s", r->pingpongPartner->spec.name.c_str());
                else                    ImGui::TextUnformatted("-");
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}


}  // namespace CustomPass
