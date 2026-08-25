#include <PCH.h>
#include "RenderDocBridge.h"

#include "renderdoc_app.h"

#include <filesystem>
#include <windows.h>

namespace RenderDocBridge
{
namespace
{
    RENDERDOC_API_1_6_0* g_api = nullptr;

    // Where capture files are written. Fixed and absolute on purpose: both
    // the user and offline analysis tooling need to find .rdc files without
    // consulting the game's working directory or MO2's VFS view.
    constexpr const wchar_t* kCaptureDir =
        L"E:\\Fallout 4 Modding\\F4SE\\logs\\renderdoc";

    std::filesystem::path ThisPluginDirectory()
    {
        HMODULE self = nullptr;
        // Resolve the module containing this function, not the EXE.
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ThisPluginDirectory),
                &self)) {
            return {};
        }
        wchar_t buffer[MAX_PATH]{};
        if (GetModuleFileNameW(self, buffer, MAX_PATH) == 0) {
            return {};
        }
        return std::filesystem::path(buffer).parent_path();
    }
}  // namespace

void Initialize()
{
    const auto pluginDir = ThisPluginDirectory();
    if (pluginDir.empty()) {
        return;
    }
    // One level DOWN from the plugin, inside the existing ShaderEngine data
    // folder. NOT next to ShaderEngineCL.dll: F4SE blindly LoadLibrary()s
    // every top-level DLL in F4SE\Plugins as a candidate plugin, which
    // armed RenderDoc at plugin-enumeration time - before ENB/upscaler hook
    // chains exist and outside this controlled init - and crashed the game
    // at startup (2026-08-15). Subdirectories are not scanned.
    const auto dllPath = pluginDir / L"ShaderEngine" / L"renderdoc.dll";

    std::error_code ec;
    if (!std::filesystem::exists(dllPath, ec)) {
        // Not an error: absence of the DLL is the OFF switch.
        return;
    }

    HMODULE module = LoadLibraryW(dllPath.c_str());
    if (!module) {
        REX::WARN(
            "RenderDocBridge: renderdoc.dll present but failed to load "
            "(error {})",
            GetLastError());
        return;
    }

    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(module, "RENDERDOC_GetAPI"));
    if (!getApi ||
        getApi(eRENDERDOC_API_Version_1_6_0,
               reinterpret_cast<void**>(&g_api)) != 1 ||
        !g_api) {
        REX::WARN("RenderDocBridge: RENDERDOC_GetAPI failed");
        g_api = nullptr;
        return;
    }

    // RenderDoc blocks vendor extensions (NvAPI) by default, and Fallout 4
    // relies on NvAPI: without this passthrough the driver removes the
    // device at the first Present on NVIDIA hardware and the game crashes
    // shortly after startup. 0x10DE is the NVIDIA vendor id. This is the
    // same fix Community Shaders ships, learned the hard way there.
    if (g_api->SetCaptureOptionU32(
            eRENDERDOC_Option_AllowUnsupportedVendorExtensions, 0x10DE) != 1) {
        REX::WARN(
            "RenderDocBridge: NvAPI passthrough rejected - expect "
            "instability on NVIDIA hardware");
    }

    std::filesystem::create_directories(kCaptureDir, ec);
    const auto captureTemplate = std::filesystem::path(kCaptureDir) / L"FO4";
    g_api->SetCaptureFilePathTemplate(captureTemplate.string().c_str());

    // Default hotkeys (F12 / PrintScreen) and the overlay stay ACTIVE on
    // purpose - the overlay is the user's confirmation that capture is
    // armed, and the hotkeys need no input plumbing of ours.

    int major = 0, minor = 0, patch = 0;
    g_api->GetAPIVersion(&major, &minor, &patch);
    REX::INFO(
        "RenderDocBridge: armed (API {}.{}.{}) - F12 captures, files go to "
        "{}\\FO4_*.rdc; remove renderdoc.dll from the plugin folder to "
        "disable",
        major, minor, patch, std::filesystem::path(kCaptureDir).string());
}
}  // namespace RenderDocBridge
