#include <Global.h>
#include <CustomPass.h>
#include <ShadowMapDeferredLighting.h>
#include <TiledDeferredLighting.h>
#include <GpuScalar.h>
#include <LightCullPolicy.h>
#include <LightTracker.h>
#include <PhaseTelemetry.h>
#include <ShadowTelemetry.h>
#include <ShadowUpgrade.h>
#include <LightSorter.h>

// Global logger pointer
std::shared_ptr<spdlog::logger> gLog;

void EnsureLogger()
{
    if (gLog) {
        spdlog::set_default_logger(gLog);
        return;
    }

    PWSTR documentsPath = nullptr;
    std::filesystem::path logPath = "ShaderEngineCL.log";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documentsPath))) {
        logPath = std::filesystem::path(documentsPath) / "My Games" / "Fallout4" / "F4SE" / "ShaderEngineCL.log";
        CoTaskMemFree(documentsPath);
        std::filesystem::create_directories(logPath.parent_path());
    }

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
    auto logger = std::make_shared<spdlog::logger>("ShaderEngineCL"s, sink);
    logger->set_level(spdlog::level::info);
    logger->flush_on(spdlog::level::info);
    logger->set_pattern("[%T] [%^%l%$] %v"s);
    spdlog::set_default_logger(logger);
    spdlog::register_logger(logger);
    gLog = logger;
    gLog->info("Logger initialized at {}", logPath.string());
}

// --- Explicit F4SE_API Definition ---
// This macro is essential for exporting functions from the DLL.
// If the F4SE headers aren't providing it correctly for your setup,
// we define it directly.
#define F4SE_API __declspec(dllexport)

// This is used by commonLibF4
namespace Version
{
    inline constexpr std::size_t MAJOR = 0;
    inline constexpr std::size_t MINOR = 1;
    inline constexpr std::size_t PATCH = 5;
    inline constexpr auto NAME = "0.1.5"sv;
    inline constexpr auto AUTHORNAME = "disi"sv;
    inline constexpr auto PROJECT = "ShaderEngineCL"sv;
} // namespace Version


// Declare the F4SEMessagingInterface and F4SEScaleformInterface
const F4SE::MessagingInterface *g_messaging = nullptr;
// Papyrus interface
const F4SE::PapyrusInterface *g_papyrus = nullptr;
// Task interface for menus and threads
const F4SE::TaskInterface *g_taskInterface = nullptr;
// Scaleform interface
const F4SE::ScaleformInterface *g_scaleformInterface = nullptr;
// Plugin handle
F4SE::PluginHandle g_pluginHandle = 0;
// Datahandler
RE::TESDataHandler *g_dataHandle = 0;

// Variables
// Global Module Name
std::string g_moduleName = "ShaderEngineCL.dll";
// Global INI Name
std::string g_iniName = "ShaderEngine.ini";
// Global Log Name
std::string g_logName = "ShaderEngineCL.log";
// Global plugin path
std::filesystem::path g_pluginPath;
// Shader folder path for loading custom shaders and watching for changes in development mode
std::filesystem::path g_shaderFolderPath;
// Global debug flag
bool DEBUGGING = false;
// Master runtime kill switch for shader replacements and custom passes.
bool SHADERENGINE_EFFECTS_ON = true;
bool SHADOW_UPGRADE_ON = false;
float DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE = 1600.0f;
// Custom buffer update flag
bool CUSTOMBUFFER_ON = true;
// Experimental directional shadow-map static-depth cache benchmark
bool SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON = false;
bool COMMAND_BUFFER_REPLAY_DEDUPE_SRV = false;
// Custom resource view slot in shader
UINT CUSTOMBUFFER_SLOT = 31;
UINT DRAWTAG_SLOT = 26;
std::vector<RaceGroupFormRef> g_raceGroupFormRefs;
std::unordered_map<std::uint32_t, std::uint32_t> g_raceGroupMaskByRaceFormID;
std::shared_mutex g_raceGroupLock;
bool g_raceGroupsResolved = false;
// Packed shader settings resource view slots
UINT MODULAR_FLOATS_SLOT = 29;
UINT MODULAR_INTS_SLOT = 28;
UINT MODULAR_BOOLS_SLOT = 27;
// G-buffer normal target index (renderTargets[]). Default is 20 = kGbufferNormal
// for the OG runtime; override in ShaderEngine.ini for other layouts. -1
// disables the `gbufferNormal` built-in input source for customPass blocks.
int NORMAL_BUFFER_INDEX = 20;
// Shader settings menu flag
bool SHADERSETTINGS_ON = false;
// Shader settings menu hotkey (default END key)
UINT SHADERSETTINGS_MENUHOTKEY = VK_END;
// Settings save hotkey (default HOME key)
UINT SHADERSETTINGS_SAVEHOTKEY = VK_HOME;
// Settings menu width
int SHADERSETTINGS_WIDTH = 600;
// Settings menu height
int SHADERSETTINGS_HEIGHT = 300;
// Settings menu opacity (0.0 - 1.0)
float SHADERSETTINGS_OPACITY = 0.75f;
// Global Development features flag (like dumping shaders, extra logging, etc)
bool DEVELOPMENT = false;
// Development GUI flag
bool DEVGUI_ON = false;
// Development GUI Width
int DEVGUI_WIDTH = 600;
// Development GUI Height
int DEVGUI_HEIGHT = 300;
// Development GUI opacity (0.0 - 1.0)
float DEVGUI_OPACITY = 0.75f;
// Global ImGui state flag
bool g_imguiInitialized = false;
// Global flag if GFX hooks are installed
bool GFX_HOOKS_INSTALLED = false;
// Shader definitions from INI
ShaderDefDB g_shaderDefinitions = {};
// Shader values from INI
GlobalShaderSettings g_shaderSettings = {};
// Shader include path for D3DCompile calls
std::filesystem::path g_commonShaderHeaderPath;
// Global INI watcher map for hot-reloading shader definitions when their files change
std::unordered_map<std::filesystem::path, std::unique_ptr<ShaderIniFileWatcher>> g_iniWatchers;
static std::atomic<bool> g_reloadQueued{false};

// Helper to check if a file exists
bool FileExists(const std::filesystem::path& filepath) {
    return std::filesystem::exists(filepath) && std::filesystem::is_regular_file(filepath);
}
// Helper to remove all whitespace from a string
inline std::string RemoveAllWhitespace(std::string line) {
    line.erase(std::remove_if(line.begin(), line.end(),
        [](unsigned char c){ return std::isspace(c); }), line.end());
    return line;
}
// Helper to remove inline comments from a line (anything after ';')
std::string RemoveInlineComment(const std::string& line) {
    // Find semicolon
    size_t pos = line.find(';');
    // Get substring before semicolon (or entire line if no semicolon)
    std::string result = (pos != std::string::npos) ? line.substr(0, pos) : line;
    // Trim trailing whitespace
    size_t end = result.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) {
        result = result.substr(0, end + 1);
    } else {
        result.clear(); // Line was all whitespace
    }
    return result;
}
// Helper function to extract key-value pair from a line
inline std::pair<std::string, std::string> GetKeyValueFromLine(const std::string& line) {
    // Remove inline comments first
    std::string cleanLine = RemoveInlineComment(line);
    if (cleanLine.empty()) {
        return {"", ""};
    }
    // Remove all whitespace for easier parsing
    cleanLine = RemoveAllWhitespace(cleanLine);
    // Find the '=' character
    size_t eqPos = cleanLine.find('=');
    if (eqPos == std::string::npos) {
        return {"", ""};
    }
    // Extract key and value
    std::string key = cleanLine.substr(0, eqPos);
    std::string value = cleanLine.substr(eqPos + 1);
    return {key, value};
}

bool ParseReplacementSRVBinding(const std::string& token, ReplacementSRVBinding& out)
{
    const size_t colon = token.find(':');
    if (colon == std::string::npos) {
        return false;
    }

    try {
        out.slot = std::stoi(token.substr(0, colon));
    } catch (...) {
        return false;
    }

    const std::string source = token.substr(colon + 1);
    const std::string lowerSource = ToLower(source);
    if (lowerSource == "depth") {
        out.kind = ReplacementSRVSourceKind::Depth;
        return true;
    }
    if (lowerSource == "scenehdr") {
        out.kind = ReplacementSRVSourceKind::SceneHDR;
        return true;
    }
    if (lowerSource == "gbuffernormal") {
        out.kind = ReplacementSRVSourceKind::GBufferNormal;
        return true;
    }
    if (lowerSource == "gbufferalbedo") {
        out.kind = ReplacementSRVSourceKind::GBufferAlbedo;
        return true;
    }
    if (lowerSource == "gbuffermaterial") {
        out.kind = ReplacementSRVSourceKind::GBufferMaterial;
        return true;
    }
    if (lowerSource == "motionvectors") {
        out.kind = ReplacementSRVSourceKind::MotionVectors;
        return true;
    }
    if (lowerSource.rfind("customresource:", 0) == 0) {
        out.kind = ReplacementSRVSourceKind::CustomResource;
        out.resourceName = source.substr(strlen("customResource:"));
        return !out.resourceName.empty();
    }
    if (lowerSource.rfind("gbufferrt:", 0) == 0) {
        out.kind = ReplacementSRVSourceKind::GBufferRT;
        try {
            out.gbufferIndex = std::stoi(source.substr(strlen("gbufferRT:")));
        } catch (...) {
            return false;
        }
        return true;
    }

    return false;
}

// Helper to get the directory of the plugin DLL OS agnostic
std::filesystem::path GetPluginDirectory(HMODULE hModule) {
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::filesystem::path fullPath(path);
    return fullPath.parent_path();
#else
    // Linux/macOS: Use dladdr() or similar
    Dl_info dl_info;
    if (dladdr((void*)GetPluginDirectory, &dl_info)) {
        std::filesystem::path fullPath(dl_info.dli_fname);
        return fullPath.parent_path();
    }
    return std::filesystem::current_path(); // Fallback
#endif
}

HMODULE GetThisModuleHandle()
{
    HMODULE hModule = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&GetThisModuleHandle),
            &hModule)) {
        hModule = GetModuleHandleA(g_moduleName.c_str());
    }
    return hModule;
}

bool SaveShaderEngineConfig(std::string* errorMessage)
{
    auto setError = [errorMessage](std::string message) {
        if (errorMessage) {
            *errorMessage = std::move(message);
        }
        return false;
    };

    try {
        std::filesystem::path configPath =
            (g_pluginPath.empty() ? std::filesystem::path{ "Data\\F4SE\\Plugins" } : g_pluginPath) / g_iniName;
        std::error_code ec;
        std::filesystem::create_directories(configPath.parent_path(), ec);
        if (ec) {
            return setError("Could not create config directory: " + ec.message());
        }

        std::vector<std::string> lines;
        if (std::filesystem::exists(configPath, ec)) {
            std::ifstream in(configPath, std::ios::in);
            if (!in.is_open()) {
                return setError("Could not open " + configPath.string() + " for reading.");
            }
            std::string line;
            while (std::getline(in, line)) {
                lines.push_back(line);
            }
        } else {
            std::istringstream defaults(defaultIni ? defaultIni : "");
            std::string line;
            while (std::getline(defaults, line)) {
                lines.push_back(line);
            }
        }

#if SHADERENGINE_ENABLE_PHASE_TELEMETRY
        bool foundPhaseTelemetry = false;
#endif
#if SHADERENGINE_ENABLE_SHADOW_TELEMETRY
        bool foundShadowTelemetry = false;
#endif
        bool foundShaderEngineEffects = false;
        bool foundShadowUpgrade = false;
        bool foundDirectionalShadowFirstCascadeDistance = false;
        bool foundShadowCache = false;
        bool foundCommandBufferReplayDedupeSrv = false;
        for (auto& line : lines) {
            auto [key, value] = GetKeyValueFromLine(line);
            const auto lowerKey = ToLower(key);
#if SHADERENGINE_ENABLE_PHASE_TELEMETRY
            if (lowerKey == "phase_telemetry_mode") {
                const bool enabled =
                    PhaseTelemetry::g_mode.load(std::memory_order_relaxed) == PhaseTelemetry::Mode::On;
                line = std::string("PHASE_TELEMETRY_MODE=") + (enabled ? "on" : "off");
                foundPhaseTelemetry = true;
            } else
#endif
#if SHADERENGINE_ENABLE_SHADOW_TELEMETRY
            if (lowerKey == "shadow_telemetry_mode") {
                const bool enabled =
                    ShadowTelemetry::g_mode.load(std::memory_order_relaxed) == ShadowTelemetry::Mode::On;
                line = std::string("SHADOW_TELEMETRY_MODE=") + (enabled ? "on" : "off");
                foundShadowTelemetry = true;
            } else
#endif
            if (lowerKey == "shaderengine_effects_on") {
                line = std::string("SHADERENGINE_EFFECTS_ON=") + (SHADERENGINE_EFFECTS_ON ? "true" : "false");
                foundShaderEngineEffects = true;
            } else if (lowerKey == "shadow_upgrade_on") {
                line = std::string("SHADOW_UPGRADE_ON=") + (SHADOW_UPGRADE_ON ? "true" : "false");
                foundShadowUpgrade = true;
            } else if (lowerKey == "directional_shadow_first_cascade_distance") {
                line = std::format(
                    "DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE={}",
                    DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE);
                foundDirectionalShadowFirstCascadeDistance = true;
            } else if (lowerKey == "shadow_cache_directional_mapslot1_on") {
                line = std::string("SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON=") + (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ? "true" : "false");
                foundShadowCache = true;
            } else if (lowerKey == "command_buffer_replay_dedupe_srv") {
                line = std::string("COMMAND_BUFFER_REPLAY_DEDUPE_SRV=") + (COMMAND_BUFFER_REPLAY_DEDUPE_SRV ? "true" : "false");
                foundCommandBufferReplayDedupeSrv = true;
            }
        }

#if SHADERENGINE_ENABLE_PHASE_TELEMETRY
        if (!foundPhaseTelemetry) {
            if (!lines.empty() && !lines.back().empty()) {
                lines.emplace_back();
            }
            lines.emplace_back("; --- PHASE TELEMETRY ---");
            lines.emplace_back("; Enable/disable per-phase timing and draw-count logging.");
            const bool enabled =
                PhaseTelemetry::g_mode.load(std::memory_order_relaxed) == PhaseTelemetry::Mode::On;
            lines.emplace_back(std::string("PHASE_TELEMETRY_MODE=") + (enabled ? "on" : "off"));
        }
#endif
#if SHADERENGINE_ENABLE_SHADOW_TELEMETRY
        if (!foundShadowTelemetry) {
            if (!lines.empty() && !lines.back().empty()) {
                lines.emplace_back();
            }
            lines.emplace_back("; --- SHADOW TELEMETRY ---");
            lines.emplace_back("; Enable/disable shadow-map timing and counter logging.");
            const bool enabled =
                ShadowTelemetry::g_mode.load(std::memory_order_relaxed) == ShadowTelemetry::Mode::On;
            lines.emplace_back(std::string("SHADOW_TELEMETRY_MODE=") + (enabled ? "on" : "off"));
        }
#endif
        if (!foundShaderEngineEffects) {
            if (!lines.empty() && !lines.back().empty()) {
                lines.emplace_back();
            }
            lines.emplace_back("; --- SHADERENGINE EFFECTS ---");
            lines.emplace_back("; Master kill switch for shader replacements and custom passes.");
            lines.emplace_back(std::string("SHADERENGINE_EFFECTS_ON=") + (SHADERENGINE_EFFECTS_ON ? "true" : "false"));
        }
        if (!foundShadowUpgrade) {
            if (!lines.empty() && !lines.back().empty()) {
                lines.emplace_back();
            }
            lines.emplace_back("; --- SHADOW UPGRADE ---");
            lines.emplace_back("; Validated OG deferred-light scope and directional-cascade patch.");
            lines.emplace_back(std::string("SHADOW_UPGRADE_ON=") + (SHADOW_UPGRADE_ON ? "true" : "false"));
        }
        if (!foundDirectionalShadowFirstCascadeDistance) {
            lines.emplace_back("; End distance of the highest-detail directional-shadow cascade.");
            lines.emplace_back("; Requires SHADOW_UPGRADE_ON=true and a restart; must be below 3000 and fDirShadowDistance.");
            lines.emplace_back(std::format(
                "DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE={}",
                DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE));
        }
        if (!foundShadowCache) {
            if (!lines.empty() && !lines.back().empty()) {
                lines.emplace_back();
            }
            lines.emplace_back("; --- SHADOW STATIC CACHE ---");
            lines.emplace_back("; Directional sun split static-depth cache A/B toggle.");
            lines.emplace_back(std::string("SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON=") + (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ? "true" : "false"));
        }
        if (!foundCommandBufferReplayDedupeSrv) {
            if (!lines.empty() && !lines.back().empty()) {
                lines.emplace_back();
            }
            lines.emplace_back("; --- COMMAND BUFFER REPLAY ---");
            lines.emplace_back("; Suppress redundant pixel-shader SRV binds during command-buffer replay.");
            lines.emplace_back(std::string("COMMAND_BUFFER_REPLAY_DEDUPE_SRV=") + (COMMAND_BUFFER_REPLAY_DEDUPE_SRV ? "true" : "false"));
        }

        std::filesystem::path tmpPath = configPath;
        tmpPath += ".tmp";
        {
            std::ofstream out(tmpPath, std::ios::out | std::ios::trunc);
            if (!out.is_open()) {
                return setError("Could not open " + tmpPath.string() + " for writing.");
            }
            for (const auto& line : lines) {
                out << line << '\n';
            }
            if (!out.good()) {
                return setError("Failed while writing " + tmpPath.string() + ".");
            }
        }

        std::error_code copyEc;
        std::filesystem::copy_file(tmpPath, configPath, std::filesystem::copy_options::overwrite_existing, copyEc);
        std::error_code removeEc;
        std::filesystem::remove(tmpPath, removeEc);
        if (copyEc) {
            return setError("Could not save " + configPath.string() + ": " + copyEc.message());
        }

        if (errorMessage) {
            errorMessage->clear();
        }
        return true;
    } catch (const std::exception& e) {
        return setError(e.what());
    } catch (...) {
        return setError("Unknown error while saving ShaderEngine.ini.");
    }
}

// Helper to parse hex string to uint32_t
uint32_t ParseHexFormID(const std::string &hexStr)
{
    return static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16));
}

std::optional<std::uint32_t> ParseRaceGroupIndex(const std::string& lowerKey)
{
    constexpr std::string_view prefix = "race_group_";
    if (lowerKey.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }

    const std::string suffix = lowerKey.substr(prefix.size());
    if (suffix.empty() ||
        !std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return std::nullopt;
    }

    const auto index = static_cast<std::uint32_t>(std::stoul(suffix));
    return index < 32 ? std::make_optional(index) : std::nullopt;
}

std::vector<RaceGroupFormRef> ParseRaceGroupFormRefs(std::uint32_t groupIndex, const std::string& value)
{
    std::vector<RaceGroupFormRef> refs;
    const std::uint32_t groupMask = 1u << groupIndex;
    std::stringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }

        const auto sep = token.find('|');
        if (sep == std::string::npos || sep == 0 || sep + 1 >= token.size()) {
            REX::WARN("LoadConfig: Invalid RACE_GROUP_{} entry '{}'", groupIndex, token);
            continue;
        }

        RaceGroupFormRef ref;
        ref.pluginName = token.substr(0, sep);
        ref.groupMask = groupMask;
        try {
            ref.formID = static_cast<std::uint32_t>(std::stoul(token.substr(sep + 1), nullptr, 0));
        } catch (...) {
            REX::WARN("LoadConfig: Invalid RACE_GROUP_{} FormID '{}'", groupIndex, token);
            continue;
        }

        refs.push_back(std::move(ref));
    }
    return refs;
}
// Helper to enumerate directories
std::vector<std::filesystem::path> GetSubdirectories(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> directories;
    if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path)) {
        return directories;
    }
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_directory()) {
            directories.push_back(entry.path());
        }
    }
    return directories;
}
// Helper to load all shader definitions from a Shader.ini file
// Returns number of definitions loaded
int LoadShaderDefinitionsFromFile(const std::filesystem::path& shaderFolderPath, const std::string& folderName) {
    std::filesystem::path iniShaderPath = shaderFolderPath / "Shader.ini";
    if (!FileExists(iniShaderPath)) {
        REX::WARN("LoadShaderDefinitionsFromFile: Shader.ini not found in: {}", shaderFolderPath.string());
        return 0;
    }
    std::ifstream file(iniShaderPath, std::ios::in);
    if (!file.is_open()) {
        REX::WARN("LoadShaderDefinitionsFromFile: Could not open Shader.ini: {}", iniShaderPath.string());
        return 0;
    }
    int loadedCount = 0;
    std::string cachedShaderID;
    bool hasCachedShaderID = false;
    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == ';') continue;
        // Remove inline comments and trim
        std::string clean = RemoveInlineComment(line);
        // Remove all whitespace for easier parsing
        clean = RemoveAllWhitespace(clean);
        if (clean.empty()) continue;
        // Check if this is a section start (e.g., [loading_screen])
        if (clean[0] == '[' && clean.back() == ']' && clean.find('/') != 0) {
            // Extract shader ID from section name
            std::string shaderId = clean.substr(1, clean.length() - 2);
            if (shaderId.empty()) {
                REX::WARN("LoadShaderDefinitionsFromFile: Empty section name in {}", iniShaderPath.string());
                continue;
            }
            // Custom passes / resources are routed to the CustomPass module.
            // They share Shader.ini files but have their own schema (see CustomPass.md).
            if (CustomPass::Registry::IsCustomSection(shaderId)) {
                std::string endTag = "[/" + shaderId + "]";
                CustomPass::g_registry.ParseSection(shaderId, file, endTag, shaderFolderPath, folderName);
                continue;
            }
            // [gpuScalar:NAME] blocks expose a single-float HLSL function's
            // return value to CPU consumers via a per-frame compute dispatch.
            // See src/GpuScalar.{h,cpp}.
            if (GpuScalar::IsGpuScalarSection(shaderId)) {
                std::string suffix = shaderId.substr(strlen("gpuScalar:"));
                std::string endTag = "[/" + shaderId + "]";
                GpuScalar::ParseSection(suffix, file, endTag, folderName);
                continue;
            }
            // Create new shader definition
            ShaderDefinition def;
            def.id = shaderId;
            // Read all properties until we hit the closing tag [/shader_id]
            std::string endTag = "[/" + shaderId + "]";
            while (std::getline(file, line)) {
                // Skip empty lines and comments
                if (line.empty() || line[0] == ';') continue;
                // Remove inline comments and trim
                std::string clean = RemoveInlineComment(line);
                // Remove all whitespace for easier parsing
                clean = RemoveAllWhitespace(clean);
                // Check for end tag
                if (ToLower(clean) == ToLower(endTag)) {
                    break;
                }
                // Check if we hit another section start (malformed INI)
                if (clean[0] == '[') {
                    REX::WARN("LoadShaderDefinitionsFromFile: Found new section before closing tag {} in {}", endTag, iniShaderPath.string());
                    break;
                }
                // Get key-value pair
                auto[key, value] = GetKeyValueFromLine(clean);
                // If key or value is empty, skip and use default values for this line
                if (key.empty() || value.empty()) continue;
                // create lower key for easier comparison
                auto lowerKey = ToLower(key);
                // Default to false
                if (lowerKey == "active") {
                    if (ToLower(value) == "true" || value == "1") {
                        def.active = true;
                    }
                }
                else if (lowerKey == "shadowupgrade") {
                    const auto enabled = ToLower(value);
                    def.shadowUpgrade = enabled == "true" || enabled == "1" || enabled == "on";
                }
                // Default to 0
                else if (lowerKey == "priority") {
                    try {
                        def.priority = std::stoi(value);
                    } catch (...) {
                        REX::WARN("LoadShaderDefinitionsFromFile: Invalid priority for {}: {}", shaderId, value);
                        def.priority = 0;
                    }
                }
                // Default to ps
                else if (lowerKey == "type") {
                    std::string type = ToLower(value);
                    if (type == "vs" || type == "vertex") {
                        def.type = ShaderType::Vertex;
                    }
                }
                // Default empty vector
                else if (lowerKey == "shaderuid") {
                    std::stringstream ss(value);
                    std::string segment;
                    while (std::getline(ss, segment, ',')) {
                        try {
                            def.shaderUID.push_back(segment);
                        } catch (...) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid shaderUID for {}: {}", shaderId, segment);
                        }
                    }
                }
                // Default to 0
                else if (lowerKey == "hash") {
                    std::stringstream ss(value);
                    std::string segment;
                    while (std::getline(ss, segment, ',')) {
                        try {
                            def.hash.push_back(ParseHexFormID(segment));
                        } catch (...) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid hash for {}: {}", shaderId, segment);
                        }
                    }
                }
                // Default to 0
                else if (lowerKey == "asmhash") {
                    std::stringstream ss(value);
                    std::string segment;
                    while (std::getline(ss, segment, ',')) {
                        try {
                            def.asmHash.push_back(ParseHexFormID(segment));
                        } catch (...) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid asmHash for {}: {}", shaderId, segment);
                        }
                    }
                }
                else if (lowerKey == "size") {
                    std::stringstream ss(value);
                    std::string segment;
                    while (std::getline(ss, segment, ',')) {
                        // Remove spaces and parentheses: ( >1024 ) -> >1024
                        segment.erase(std::remove(segment.begin(), segment.end(), ' '), segment.end());
                        segment.erase(std::remove(segment.begin(), segment.end(), '('), segment.end());
                        segment.erase(std::remove(segment.begin(), segment.end(), ')'), segment.end());
                        SizeRequirement req;
                        if (segment.find('>') != std::string::npos) {
                            req.op = SizeOp::Greater;
                            req.value = std::stoull(segment.substr(segment.find('>') + 1));
                        } else if (segment.find('<') != std::string::npos) {
                            req.op = SizeOp::Less;
                            req.value = std::stoull(segment.substr(segment.find('<') + 1));
                        } else {
                            req.op = SizeOp::Equal;
                            req.value = std::stoull(segment);
                        }
                        def.sizeRequirements.push_back(req);
                    }
                }
                else if (lowerKey == "buffersize") {
                    std::istringstream ss(value);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        size_t atPos = token.find('@');
                        if (atPos != std::string::npos) {
                            try {
                                int size = std::stoi(token.substr(0, atPos));
                                std::string slotStr = token.substr(atPos + 1);
                                // Trim whitespace from slot string
                                slotStr.erase(0, slotStr.find_first_not_of(" \t"));
                                slotStr.erase(slotStr.find_last_not_of(" \t") + 1);
                                // If slot is empty, use -1 for "any slot"
                                int slot = slotStr.empty() ? -1 : std::stoi(slotStr);
                                def.bufferSizes.emplace_back(size, slot);
                            } catch (...) {
                                REX::WARN("LoadConfig: Invalid or empty buffer size in INI: {}", value);
                                def.bufferSizes.clear();
                            }
                        }
                    }
                }
                else if (lowerKey == "textures") {
                    std::istringstream ss(value);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        def.textureSlots.push_back(std::stoi(token));
                    }
                    // Update textureSlotMask bitmask based on specified texture slots
                    if (def.textureSlots.size() > 0) {
                        for (const auto& tex : def.textureSlots) {
                            int slot = tex;
                            if (slot >= 0 && slot < 32) {
                                def.textureSlotMask |= (std::uint32_t(1) << slot);
                            }
                        }
                    } else {
                        def.textureSlotMask = 0;
                    }
                }
                else if (lowerKey == "texturedimensions") {
                    std::istringstream ss(value);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        size_t atPos = token.find('@');
                        if (atPos != std::string::npos) {
                            try {
                                // Shader.ini uses dimension@slot, matching the
                                // generated metadata written by ShaderPipeline.
                                int dimension = std::stoi(token.substr(0, atPos));
                                std::string slotStr = token.substr(atPos + 1);
                                // Trim whitespace from slot string
                                slotStr.erase(0, slotStr.find_first_not_of(" \t"));
                                slotStr.erase(slotStr.find_last_not_of(" \t") + 1);
                                int slot = slotStr.empty() ? -1 : std::stoi(slotStr);
                                def.textureDimensions.emplace_back(dimension, slot);
                                if (dimension >= 0 && dimension < 32) {
                                    def.textureDimensionMask |= (1UL << dimension);
                                }
                            } catch (...) {
                                REX::WARN("LoadConfig: Invalid or empty texture dimension in INI: {}", token);
                                def.textureDimensions.clear();
                            }
                        }
                    }
                    // Update textureDimensionMask bitmask based on specified texture dimensions
                    if (def.textureDimensions.size() > 0) {
                        for (const auto& tex : def.textureDimensions) {
                            int dimension = tex.first;
                            if (dimension >= 0 && dimension < 32) {
                                def.textureDimensionMask |= (std::uint32_t(1) << dimension);
                            }
                        }
                    } else {
                        def.textureDimensionMask = 0;
                    }
                }
                // Default to 0
                else if (lowerKey == "textureslotmask") {
                    try {
                        def.textureSlotMask = std::stoul(value, nullptr, 16);
                    } catch (...) {
                        REX::WARN("LoadShaderDefinitionsFromFile: Invalid textureSlotMask for {}: {}", shaderId, value);
                        def.textureSlotMask = 0;
                    }
                }
                // Default to 0
                else if (lowerKey == "texturedimensionmask") {
                    try {
                        def.textureDimensionMask = std::stoul(value, nullptr, 16);
                    } catch (...) {
                        REX::WARN("LoadShaderDefinitionsFromFile: Invalid textureDimensionMask for {}: {}", shaderId, value);
                        def.textureDimensionMask = 0;
                    }
                }
                else if (lowerKey == "inputtexturecount") {
                    std::stringstream ss(value);
                    std::string segment;
                    while (std::getline(ss, segment, ',')) {
                        // Remove spaces and parentheses: ( >2 ) -> >2
                        segment.erase(std::remove(segment.begin(), segment.end(), ' '), segment.end());
                        segment.erase(std::remove(segment.begin(), segment.end(), '('), segment.end());
                        segment.erase(std::remove(segment.begin(), segment.end(), ')'), segment.end());
                        InputCountRequirement req;
                        if (segment.find('>') != std::string::npos) {
                            req.op = SizeOp::Greater;
                            req.value = std::stoi(segment.substr(segment.find('>') + 1));
                        } else if (segment.find('<') != std::string::npos) {
                            req.op = SizeOp::Less;
                            req.value = std::stoi(segment.substr(segment.find('<') + 1));
                        } else {
                            req.op = SizeOp::Equal;
                            req.value = std::stoi(segment);
                        }
                        def.inputTextureCountRequirements.push_back(req);
                    }
                }
                else if (lowerKey == "inputcount") {
                    std::stringstream ss(value);
                    std::string segment;
                    while (std::getline(ss, segment, ',')) {
                        // Remove spaces and parentheses: ( >2 ) -> >2
                        segment.erase(std::remove(segment.begin(), segment.end(), ' '), segment.end());
                        segment.erase(std::remove(segment.begin(), segment.end(), '('), segment.end());
                        segment.erase(std::remove(segment.begin(), segment.end(), ')'), segment.end());
                        InputCountRequirement req;
                        if (segment.find('>') != std::string::npos) {
                            req.op = SizeOp::Greater;
                            req.value = std::stoi(segment.substr(segment.find('>') + 1));
                        } else if (segment.find('<') != std::string::npos) {
                            req.op = SizeOp::Less;
                            req.value = std::stoi(segment.substr(segment.find('<') + 1));
                        } else {
                            req.op = SizeOp::Equal;
                            req.value = std::stoi(segment);
                        }
                        def.inputCountRequirements.push_back(req);
                    }
                }
                else if (lowerKey == "inputmask") {
                    try {
                        def.inputMask = std::stoul(value, nullptr, 16);
                    } catch (...) {
                        REX::WARN("LoadShaderDefinitionsFromFile: Invalid inputMask for {}: {}", shaderId, value);
                        def.inputMask = 0;
                    }
                }
                else if (lowerKey == "outputcount") {
                    std::stringstream ss(value);
                    std::string segment;
                    while (std::getline(ss, segment, ',')) {
                        // Remove spaces and parentheses: ( >2 ) -> >2
                        segment.erase(std::remove(segment.begin(), segment.end(), ' '), segment.end());
                        segment.erase(std::remove(segment.begin(), segment.end(), '('), segment.end());
                        segment.erase(std::remove(segment.begin(), segment.end(), ')'), segment.end());
                        OutputCountRequirement req;
                        if (segment.find('>') != std::string::npos) {
                            req.op = SizeOp::Greater;
                            req.value = std::stoi(segment.substr(segment.find('>') + 1));
                        } else if (segment.find('<') != std::string::npos) {
                            req.op = SizeOp::Less;
                            req.value = std::stoi(segment.substr(segment.find('<') + 1));
                        } else {
                            req.op = SizeOp::Equal;
                            req.value = std::stoi(segment);
                        }
                        def.outputCountRequirements.push_back(req);
                    }
                }
                else if (lowerKey == "outputmask") {
                    try {
                        def.outputMask = std::stoul(value, nullptr, 16);
                    } catch (...) {
                        REX::WARN("LoadShaderDefinitionsFromFile: Invalid outputMask for {}: {}", shaderId, value);
                        def.outputMask = 0;
                    }
                }
                // Default to empty string
                else if (lowerKey == "shader") {
                    // Resolve shader path relative to shader folder
                    std::filesystem::path shaderPath = shaderFolderPath / value;
                    if (!FileExists(shaderPath)) {
                        REX::WARN("LoadShaderDefinitionsFromFile: Shader file not found for {}: {}", shaderId, shaderPath.string());
                        def.shaderFile = "";
                    } else {
                        def.shaderFile = shaderPath;
                    }
                }
                // Bind file-backed textures whenever this definition's replacement shader is active.
                // Format: bindTexture=40:foo.dds,41:bar.png. Paths are relative to this shader folder.
                else if (lowerKey == "bindtexture" || lowerKey == "replacementtexture" || lowerKey == "shadertexture") {
                    std::istringstream ss(value);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        const size_t colon = token.find(':');
                        if (colon == std::string::npos) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid bindTexture entry for {}: {}", shaderId, token);
                            continue;
                        }

                        ReplacementTextureBinding binding{};
                        try {
                            binding.slot = std::stoi(token.substr(0, colon));
                        } catch (...) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid bindTexture slot for {}: {}", shaderId, token);
                            continue;
                        }

                        std::filesystem::path texturePath = token.substr(colon + 1);
                        if (texturePath.is_relative()) {
                            texturePath = shaderFolderPath / texturePath;
                        }
                        binding.file = texturePath;
                        def.replacementTextures.push_back(std::move(binding));
                    }
                }
                // Bind engine/custom SRVs whenever this definition's replacement shader is active.
                // Format: bindSRV=40:customResource:ssaoFinal,68:sceneHDR.
                else if (lowerKey == "bindsrv" || lowerKey == "shaderresource" || lowerKey == "replacementsrv") {
                    std::istringstream ss(value);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        ReplacementSRVBinding binding{};
                        if (!ParseReplacementSRVBinding(token, binding)) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid bindSRV entry for {}: {}", shaderId, token);
                            continue;
                        }
                        if (binding.slot < 0 || binding.slot >= 128) {
                            REX::WARN("LoadShaderDefinitionsFromFile: bindSRV slot out of range for {}: {}", shaderId, token);
                            continue;
                        }
                        def.replacementSRVs.push_back(std::move(binding));
                    }
                }
                // Default to false
                else if (lowerKey == "log") {
                    if (ToLower(value) == "true" || value == "1") {
                        def.log = true;
                    }
                }
                // Default to false
                else if (lowerKey == "dump") {
                    if (ToLower(value) == "true" || value == "1") {
                        def.dump = true;
                    }
                }
                // Optional reference to a float value (declared in this folder's
                // Values.ini, or any [global] block) whose current value scales
                // the engine's BSLight cull bound while the rule is active.
                // See LightCullPolicy.{h,cpp}. Default empty (no scaling).
                else if (lowerKey == "lightcullradiusscalevalue") {
                    def.lightCullRadiusScaleValue = value;
                }
            }
            // Cache the ShaderDefinition ID
            std::string shaderID = def.id;
            if (!hasCachedShaderID) {
                cachedShaderID = shaderID;
                hasCachedShaderID = true;
            }
            // Add the definition to global list
            g_shaderDefinitions.AddDefinition(new ShaderDefinition(std::move(def)));
            loadedCount++;
            REX::INFO("LoadShaderDefinitionsFromFile: Loaded shader '{}' from {}/Shader.ini", shaderID, folderName);
        }
    }
    file.close();
    // Load Values.ini once per folder and assign values to the first loaded definition ID
    std::filesystem::path iniValuesPath = shaderFolderPath / "Values.ini";
    if (!FileExists(iniValuesPath)) {
        REX::WARN("LoadShaderDefinitionsFromFile: Values.ini not found in: {}", shaderFolderPath.string());
    }
    std::ifstream valuesFile(iniValuesPath, std::ios::in);
    if (!valuesFile.is_open()) {
        REX::WARN("LoadShaderDefinitionsFromFile: Could not open Values.ini: {}", iniValuesPath.string());
    } else if (!hasCachedShaderID) {
        REX::WARN("LoadShaderDefinitionsFromFile: No shader definition loaded in {}. Skipping Values.ini", shaderFolderPath.string());
    } else {
        std::string valueLine;
        while (std::getline(valuesFile, valueLine)) {
            // Create new shader value
            ShaderValue shaderV;
            // Skip empty lines and comments
            if (valueLine.empty() || valueLine[0] == ';') continue;
            // Remove inline comments and trim
            std::string clean = RemoveInlineComment(valueLine);
            // Remove all whitespace for easier parsing
            clean = RemoveAllWhitespace(clean);
            if (clean.empty()) continue;
            // Check if this is a section start (e.g., [global] or [local])
            if (clean[0] == '[' && clean.back() == ']' && clean.find('/') != 0) {
                // Extract value scope from section name
                std::string valueScope = clean.substr(1, clean.length() - 2);
                if (valueScope.empty()) {
                    REX::WARN("LoadShaderDefinitionsFromFile: Empty value scope in {}", iniValuesPath.string());
                    continue;
                }
                shaderV.shaderDefinitionId = cachedShaderID;
                shaderV.folderName = folderName;
                // Read all properties until we hit the closing tag [/scope]
                std::string endTag = "[/" + valueScope + "]";
                // Check if global values section
                if (ToLower(valueScope) == "global") {
                    shaderV.global = true;
                }
                while (std::getline(valuesFile, valueLine)) {
                    // Skip empty lines and comments
                    if (valueLine.empty() || valueLine[0] == ';') continue;
                    // Remove inline comments and trim
                    std::string valueClean = RemoveInlineComment(valueLine);
                    // Remove all whitespace for easier parsing
                    valueClean = RemoveAllWhitespace(valueClean);
                    if (valueClean.empty()) continue;
                    // Check for end tag
                    if (ToLower(valueClean) == ToLower(endTag)) {
                        break;
                    }
                    // Check if we hit another section start (malformed INI)
                    if (valueClean[0] == '[') {
                        REX::WARN("LoadShaderDefinitionsFromFile: Found new value before closing tag {} in {}", endTag, iniValuesPath.string());
                        break;
                    }
                    // Get key-value pair
                    auto[key, value] = GetKeyValueFromLine(valueClean);
                    // If key or value is empty, skip and use default values for this line
                    if (key.empty() || value.empty()) continue;
                    // create lower key for easier comparison
                    auto lowerKey = ToLower(key);
                    if (lowerKey == "id") {
                        shaderV.id = value;
                    }
                    else if (lowerKey == "label") {
                        shaderV.label = value;
                    }
                    else if (lowerKey == "group") {
                        shaderV.group = value;
                    }
                    else if (lowerKey == "type") {
                        std::string type = ToLower(value);
                        if (type == "bool") {
                            shaderV.type = ShaderValue::Type::Bool;
                        }
                        else if (type == "int") {
                            shaderV.type = ShaderValue::Type::Int;
                        }
                        else if (type == "float") {
                            shaderV.type = ShaderValue::Type::Float;
                        }
                    }
                    else if (lowerKey == "value") {
                        try {
                            if (shaderV.type == ShaderValue::Type::Bool) {
                                shaderV.current.b = (ToLower(value) == "true" || value == "1");
                                shaderV.def.b = (ToLower(value) == "true" || value == "1");
                            }
                            else if (shaderV.type == ShaderValue::Type::Int) {
                                shaderV.current.i = std::stoi(value);
                                shaderV.def.i = std::stoi(value);
                            }
                            else if (shaderV.type == ShaderValue::Type::Float) {
                                shaderV.current.f = std::stof(value);
                                shaderV.def.f = std::stof(value);
                            }
                        } catch (...) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid value for {}: {}", shaderV.id, value);
                        }
                    }
                    else if (lowerKey == "min") {
                        try {
                            if (shaderV.type == ShaderValue::Type::Int) {
                                shaderV.min.i = std::stoi(value);
                            }
                            else if (shaderV.type == ShaderValue::Type::Float) {
                                shaderV.min.f = std::stof(value);
                            }
                        } catch (...) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid min value for {}: {}", shaderV.id, value);
                        }
                    }
                    else if (lowerKey == "max") {
                        try {
                            if (shaderV.type == ShaderValue::Type::Int) {
                                shaderV.max.i = std::stoi(value);
                            }
                            else if (shaderV.type == ShaderValue::Type::Float) {
                                shaderV.max.f = std::stof(value);
                            }
                        } catch (...) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid max value for {}: {}", shaderV.id, value);
                        }
                    }
                    else if (lowerKey == "step") {
                        try {
                            if (shaderV.type == ShaderValue::Type::Int) {
                                shaderV.step.i = std::stoi(value);
                            }
                            else if (shaderV.type == ShaderValue::Type::Float) {
                                shaderV.step.f = std::stof(value);
                            }
                        } catch (...) {
                            REX::WARN("LoadShaderDefinitionsFromFile: Invalid step value for {}: {}", shaderV.id, value);
                        }
                    }
                }
                ShaderValue finalShaderV = shaderV; // Make a copy for ownership handoff
                g_shaderSettings.AddShaderValue(new ShaderValue(std::move(finalShaderV)));
            }
        }
        REX::INFO("LoadShaderDefinitionsFromFile: Loaded values for shader '{}' from {}/Values.ini", cachedShaderID, folderName);
    }
    valuesFile.close();
    // After loading all definitions from this INI file, create a watcher for it
    if (DEVELOPMENT && loadedCount > 0) {
        auto watcher = std::make_unique<ShaderIniFileWatcher>(iniShaderPath, folderName);
        watcher->Start();
        g_iniWatchers[iniShaderPath] = std::move(watcher);
    }
    return loadedCount;
}

// Load config and shader definitions from INI file
void LoadConfig(HMODULE hModule) {
    std::filesystem::path configPath = std::filesystem::path{ "Data\\F4SE\\Plugins" } / g_iniName;
    g_pluginPath = configPath.parent_path();
    std::string configPathStr = configPath.string();
    REX::INFO("LoadConfig: Loading config from: {}", configPathStr);
    std::ifstream file(configPathStr, std::ios::in);
    if (!file.is_open()) {
        REX::WARN("LoadConfig: Could not open INI file: {}. Creating default.", configPathStr);
        // Create default INI file with debugging enabled if it doesn't exist
        std::ofstream out(configPathStr, std::ios::out);
        if (out.is_open()) {
            out << defaultIni;
            out.close();
        }
        file.open(configPathStr, std::ios::in);
    }
    // Parse main INI file for global settings
    std::string line;
    std::vector<RaceGroupFormRef> parsedRaceGroupRefs;
    while (std::getline(file, line)) {
        auto[key, value] = GetKeyValueFromLine(line);
        if (key.empty()) continue;
        // create lower key for easier comparison
        auto lowerKey = ToLower(key);
        if (lowerKey == "debugging") {
            DEBUGGING = (ToLower(value) == "true" || value == "1");
            REX::INFO("LoadConfig: DEBUGGING set to {}", DEBUGGING);
            continue;
        }
        else if (lowerKey == "custombuffer_on") {
            CUSTOMBUFFER_ON = (ToLower(value) == "true" || value == "1");
            REX::INFO("LoadConfig: CUSTOMBUFFER_ON set to {}", CUSTOMBUFFER_ON);
            continue;
        }
        else if (lowerKey == "shaderengine_effects_on") {
            const std::string v = ToLower(value);
            SHADERENGINE_EFFECTS_ON = (v == "true" || v == "1" || v == "on");
            REX::INFO("LoadConfig: SHADERENGINE_EFFECTS_ON set to {}", SHADERENGINE_EFFECTS_ON);
            continue;
        }
        else if (lowerKey == "shadow_upgrade_on") {
            const std::string v = ToLower(value);
            SHADOW_UPGRADE_ON = (v == "true" || v == "1" || v == "on");
            REX::INFO("LoadConfig: SHADOW_UPGRADE_ON set to {}", SHADOW_UPGRADE_ON);
            continue;
        }
        else if (lowerKey == "directional_shadow_first_cascade_distance") {
            try {
                const auto distance = std::stof(value);
                if (!std::isfinite(distance) || distance <= 0.0f) {
                    REX::WARN(
                        "LoadConfig: DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE "
                        "must be finite and positive; keeping {}",
                        DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE);
                } else {
                    DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE = distance;
                    REX::INFO(
                        "LoadConfig: DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE "
                        "set to {}",
                        DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE);
                }
            } catch (...) {
                REX::WARN(
                    "LoadConfig: Invalid "
                    "DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE value: {}; "
                    "keeping {}",
                    value,
                    DIRECTIONAL_SHADOW_FIRST_CASCADE_DISTANCE);
            }
            continue;
        }
        else if (lowerKey == "shadow_cache_directional_mapslot1_on") {
            const std::string v = ToLower(value);
            SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON = (v == "true" || v == "1" || v == "on");
            REX::INFO("LoadConfig: SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON set to {}", SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON);
            continue;
        }
        else if (lowerKey == "command_buffer_replay_dedupe_srv") {
            const std::string v = ToLower(value);
            COMMAND_BUFFER_REPLAY_DEDUPE_SRV = (v == "true" || v == "1" || v == "on");
            REX::INFO("LoadConfig: COMMAND_BUFFER_REPLAY_DEDUPE_SRV set to {}", COMMAND_BUFFER_REPLAY_DEDUPE_SRV);
            continue;
        }
        else if (lowerKey == "shadow_cache_directional_mapslot1_max_skip") {
            REX::WARN("LoadConfig: SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_MAX_SKIP is deprecated and ignored");
            continue;
        }
        else if (lowerKey == "custombuffer_slot") {
            try {
                CUSTOMBUFFER_SLOT = static_cast<UINT>(std::stoi(value));
                REX::INFO("LoadConfig: CUSTOMBUFFER_SLOT set to {}", CUSTOMBUFFER_SLOT);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid CUSTOMBUFFER_SLOT value: {}. Using default: {}", value, CUSTOMBUFFER_SLOT);
            }
            continue;
        }
        else if (lowerKey == "normal_buffer_index") {
            try {
                NORMAL_BUFFER_INDEX = std::stoi(value);
                REX::INFO("LoadConfig: NORMAL_BUFFER_INDEX set to {}", NORMAL_BUFFER_INDEX);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid NORMAL_BUFFER_INDEX value: {}. Using default: {}", value, NORMAL_BUFFER_INDEX);
            }
            continue;
        }
        else if (lowerKey == "drawtag_slot") {
            try {
                DRAWTAG_SLOT = static_cast<UINT>(std::stoi(value));
                REX::INFO("LoadConfig: DRAWTAG_SLOT set to {}", DRAWTAG_SLOT);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid DRAWTAG_SLOT value: {}. Using default: {}", value, DRAWTAG_SLOT);
            }
            continue;
        }
        else if (auto groupIndex = ParseRaceGroupIndex(lowerKey)) {
            auto refs = ParseRaceGroupFormRefs(*groupIndex, value);
            REX::INFO("LoadConfig: RACE_GROUP_{} loaded {} entr{}", *groupIndex, refs.size(), refs.size() == 1 ? "y" : "ies");
            parsedRaceGroupRefs.insert(
                parsedRaceGroupRefs.end(),
                std::make_move_iterator(refs.begin()),
                std::make_move_iterator(refs.end()));
            continue;
        }
        else if (lowerKey == "modular_floats_slot") {
            try {
                MODULAR_FLOATS_SLOT = static_cast<UINT>(std::stoi(value));
                REX::INFO("LoadConfig: MODULAR_FLOATS_SLOT set to {}", MODULAR_FLOATS_SLOT);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid MODULAR_FLOATS_SLOT value: {}. Using default: {}", value, MODULAR_FLOATS_SLOT);
            }
            continue;
        }
        else if (lowerKey == "modular_ints_slot") {
            try {
                MODULAR_INTS_SLOT = static_cast<UINT>(std::stoi(value));
                REX::INFO("LoadConfig: MODULAR_INTS_SLOT set to {}", MODULAR_INTS_SLOT);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid MODULAR_INTS_SLOT value: {}. Using default: {}", value, MODULAR_INTS_SLOT);
            }
            continue;
        }
        else if (lowerKey == "modular_bools_slot") {
            try {
                MODULAR_BOOLS_SLOT = static_cast<UINT>(std::stoi(value));
                REX::INFO("LoadConfig: MODULAR_BOOLS_SLOT set to {}", MODULAR_BOOLS_SLOT);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid MODULAR_BOOLS_SLOT value: {}. Using default: {}", value, MODULAR_BOOLS_SLOT);
            }
            continue;
        }
        else if (lowerKey == "shadersettings_on") {
            SHADERSETTINGS_ON = (ToLower(value) == "true" || value == "1");
            REX::INFO("LoadConfig: SHADERSETTINGS_ON set to {}", SHADERSETTINGS_ON);
            continue;
        }
        else if (lowerKey == "shadersettings_menuhotkey") {
            try {
                SHADERSETTINGS_MENUHOTKEY = static_cast<UINT>(std::stoi(value, nullptr, 16));
                REX::INFO("LoadConfig: SHADERSETTINGS_MENUHOTKEY set to {}", SHADERSETTINGS_MENUHOTKEY);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid SHADERSETTINGS_MENUHOTKEY value: {}. Using default: {}", value, SHADERSETTINGS_MENUHOTKEY);
            }
            continue;
        }
        else if (lowerKey == "shadersettings_savehotkey") {
            try {
                SHADERSETTINGS_SAVEHOTKEY = static_cast<UINT>(std::stoi(value, nullptr, 16));
                REX::INFO("LoadConfig: SHADERSETTINGS_SAVEHOTKEY set to {}", SHADERSETTINGS_SAVEHOTKEY);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid SHADERSETTINGS_SAVEHOTKEY value: {}. Using default: {}", value, SHADERSETTINGS_SAVEHOTKEY);
            }
            continue;
        }
        else if (lowerKey == "shadersettings_width") {
            try {
                SHADERSETTINGS_WIDTH = std::stoi(value);
                REX::INFO("LoadConfig: SHADERSETTINGS_WIDTH set to {}", SHADERSETTINGS_WIDTH);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid SHADERSETTINGS_WIDTH value: {}. Using default: {}", value, SHADERSETTINGS_WIDTH);
            }
            continue;
        }
        else if (lowerKey == "shadersettings_height") {
            try {
                SHADERSETTINGS_HEIGHT = std::stoi(value);
                REX::INFO("LoadConfig: SHADERSETTINGS_HEIGHT set to {}", SHADERSETTINGS_HEIGHT);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid SHADERSETTINGS_HEIGHT value: {}. Using default: {}", value, SHADERSETTINGS_HEIGHT);
            }
            continue;
        }
        else if (lowerKey == "shadersettings_opacity") {
            try {
                SHADERSETTINGS_OPACITY = std::stof(value);
                if (SHADERSETTINGS_OPACITY < 0.0f || SHADERSETTINGS_OPACITY > 1.0f) {
                    REX::WARN("LoadConfig: SHADERSETTINGS_OPACITY value out of range (0.0 - 1.0): {}. Using default: {}", value, SHADERSETTINGS_OPACITY);
                    SHADERSETTINGS_OPACITY = 0.75f;
                } else {
                    REX::INFO("LoadConfig: SHADERSETTINGS_OPACITY set to {}", SHADERSETTINGS_OPACITY);
                }
            } catch (...) {
                REX::WARN("LoadConfig: Invalid SHADERSETTINGS_OPACITY value: {}. Using default: {}", value, SHADERSETTINGS_OPACITY);
            }
            continue;
        }
        else if (lowerKey == "development") {
            DEVELOPMENT = (ToLower(value) == "true" || value == "1");
            REX::INFO("LoadConfig: DEVELOPMENT set to {}", DEVELOPMENT);
            continue;
        }
        else if (lowerKey == "devgui_on") {
            DEVGUI_ON = (ToLower(value) == "true" || value == "1");
            REX::INFO("LoadConfig: DEVGUI_ON set to {}", DEVGUI_ON);
            continue;
        }
        else if (lowerKey == "devgui_width") {
            try {
                DEVGUI_WIDTH = std::stoi(value);
                REX::INFO("LoadConfig: DEVGUI_WIDTH set to {}", DEVGUI_WIDTH);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid DEVGUI_WIDTH value: {}. Using default: {}", value, DEVGUI_WIDTH);
            }
            continue;
        }
        else if (lowerKey == "devgui_height") {
            try {
                DEVGUI_HEIGHT = std::stoi(value);
                REX::INFO("LoadConfig: DEVGUI_HEIGHT set to {}", DEVGUI_HEIGHT);
            } catch (...) {
                REX::WARN("LoadConfig: Invalid DEVGUI_HEIGHT value: {}. Using default: {}", value, DEVGUI_HEIGHT);
            }
            continue;
        }
        else if (lowerKey == "devgui_opacity") {
            try {
                DEVGUI_OPACITY = std::stof(value);
                if (DEVGUI_OPACITY < 0.0f || DEVGUI_OPACITY > 1.0f) {
                    REX::WARN("LoadConfig: DEVGUI_OPACITY value out of range (0.0 - 1.0): {}. Using default: {}", value, DEVGUI_OPACITY);
                    DEVGUI_OPACITY = 0.5f;
                } else {
                    REX::INFO("LoadConfig: DEVGUI_OPACITY set to {}", DEVGUI_OPACITY);
                }
            } catch (...) {
                REX::WARN("LoadConfig: Invalid DEVGUI_OPACITY value: {}. Using default: {}", value, DEVGUI_OPACITY);
            }
            continue;
        }
        else if (lowerKey == "light_sorter_mode") {
            const std::string v = ToLower(value);
            if (v == "on" || v == "true" || v == "1") {
                LightSorter::g_mode.store(LightSorter::Mode::On, std::memory_order_relaxed);
                REX::INFO("LoadConfig: LIGHT_SORTER_MODE set to on");
            } else {
                LightSorter::g_mode.store(LightSorter::Mode::Off, std::memory_order_relaxed);
                REX::INFO("LoadConfig: LIGHT_SORTER_MODE set to off");
            }
            continue;
        }
#if SHADERENGINE_ENABLE_PHASE_TELEMETRY
        else if (lowerKey == "phase_telemetry_mode") {
            const std::string v = ToLower(value);
            if (v == "on" || v == "true" || v == "1") {
                PhaseTelemetry::g_mode.store(PhaseTelemetry::Mode::On, std::memory_order_relaxed);
                REX::INFO("LoadConfig: PHASE_TELEMETRY_MODE set to on");
            } else {
                PhaseTelemetry::g_mode.store(PhaseTelemetry::Mode::Off, std::memory_order_relaxed);
                REX::INFO("LoadConfig: PHASE_TELEMETRY_MODE set to off");
            }
            continue;
        }
#endif
#if SHADERENGINE_ENABLE_SHADOW_TELEMETRY
        else if (lowerKey == "shadow_telemetry_mode") {
            const std::string v = ToLower(value);
            if (v == "on" || v == "true" || v == "1") {
                ShadowTelemetry::g_mode.store(ShadowTelemetry::Mode::On, std::memory_order_relaxed);
                REX::INFO("LoadConfig: SHADOW_TELEMETRY_MODE set to on");
            } else {
                ShadowTelemetry::g_mode.store(ShadowTelemetry::Mode::Off, std::memory_order_relaxed);
                REX::INFO("LoadConfig: SHADOW_TELEMETRY_MODE set to off");
            }
            continue;
        }
#endif
    }
    file.close();
    {
        std::unique_lock lock(g_raceGroupLock);
        g_raceGroupFormRefs = std::move(parsedRaceGroupRefs);
        g_raceGroupMaskByRaceFormID.clear();
        g_raceGroupsResolved = false;
        REX::INFO("LoadConfig: queued {} race group form reference(s)", g_raceGroupFormRefs.size());
    }
    // Scan for shader definitions in subdirectories
    if (g_shaderFolderPath.empty()) {
        g_shaderFolderPath = configPath.parent_path() / "ShaderEngine";
    }
    REX::INFO("LoadConfig: Scanning for shaders in: {}", g_shaderFolderPath.string());
    auto shaderFolders = GetSubdirectories(g_shaderFolderPath);
    REX::INFO("LoadConfig: Found {} shader folder(s)", shaderFolders.size());
    int totalDefinitions = 0;
    for (const auto& folderPath : shaderFolders) {
        std::string folderName = folderPath.filename().string();
        int count = LoadShaderDefinitionsFromFile(folderPath, folderName);
        totalDefinitions += count;
        if (count > 0) {
            REX::INFO("LoadConfig: Loaded {} definition(s) from {}", count, folderName);
        }
    }
    // Resolve `triggerHookId=...` references on customPass blocks now that all
    // [shaderId] definitions are known.
    CustomPass::g_registry.ResolveHookIdTriggers();
    // Load stored shader settings values from disk
    g_shaderSettings.LoadSettings();
    REX::INFO("LoadConfig: Completed. Loaded {} shader definition(s) total", totalDefinitions);
    // Add HlslFileWatcher for shaderBasePath to automatically reload shader definitions on changes
    if (DEVELOPMENT) {
        REX::INFO("LoadConfig: Setting up development file watcher for shader definitions in: {}", g_shaderFolderPath.string());
        for (auto* def : g_shaderDefinitions.definitions) {
            if (def->active && !def->shaderFile.empty()) {
                def->hlslFileWatcher = std::make_unique<HlslFileWatcher>(def->shaderFile);
                def->hlslFileWatcher->Start();
            }
        }
        // CustomPass shaders get their own per-pass watchers ? without these
        // the SSRTGI / denoise / composite shaders would only recompile on a
        // full Shader.ini reload, which is painful for iteration.
        CustomPass::g_registry.StartFileWatchers();
    }
    // Set global shader include path for D3DCompile calls
    try {
    g_commonShaderHeaderPath = g_shaderFolderPath / "Include";
    } catch (...) {
        // Fallback if canonical fails
        g_commonShaderHeaderPath = configPath.parent_path() / "ShaderEngine" / "Include";
    }
    if (DEBUGGING) {
        REX::INFO("LoadConfig: Setting global shader include path for D3DCompile to: {}", g_commonShaderHeaderPath.string());
    }
    // Watch the common include dir so an edit to any file under Include/
    // invalidates the ShaderCache include memo before the next compile reads
    // it. Without this, editing only an include leaves running shaders on
    // stale bytecode AND poisons the on-disk cache the next time any
    // dependent shader is recompiled. Only spun up in DEVELOPMENT mode ?
    // the include dir is immutable in production.
    if (DEVELOPMENT && !g_includeDirWatcher) {
        g_includeDirWatcher = std::make_unique<IncludeDirWatcher>(g_commonShaderHeaderPath);
        g_includeDirWatcher->Start();
    }
    // Spin up the background precompile worker and queue every active def
    // and every customPass. The worker waits for the renderer device before
    // doing real work, so it's safe to enqueue here even though D3D isn't
    // ready yet at plugin load. By the time the engine starts binding
    // shaders, most replacement bytecode is either cache-loaded or freshly
    // compiled and waiting in def->loadedPixelShader / loadedVertexShader,
    // and customPass psShader/csShader are already populated for FirePass.
    if (!g_precompileWorker) g_precompileWorker = std::make_unique<PrecompileWorker>();
    g_precompileWorker->Start();
    {
        std::shared_lock lk(g_shaderDefinitions.mutex);
        for (auto* def : g_shaderDefinitions.definitions) {
            if (def && def->active && !def->shaderFile.empty()) {
                g_precompileWorker->Enqueue(def->id, [def]{ CompileShader_Internal(def); });
            }
        }
    }
    CustomPass::g_registry.EnqueuePrecompileJobs();
}

// Hot-reload all shader definitions and rematch shaders in ShaderDB
void ReloadAllShaderDefinitions_Internal() {
    if (DEBUGGING) {
        REX::INFO("ReloadAllShaderDefinitions_Internal: Starting hot reload of shader definitions...");
    }
    // Stop the precompile worker before we touch any def: queue is drained
    // and the worker thread is joined, so by the time we hit Clear() below
    // there's no chance of a worker-thread CompileShader_Internal touching
    // a deleted ShaderDefinition*.
    if (g_precompileWorker) g_precompileWorker->Stop();
    // Drop the include-dir hash and include-file content memo so the next
    // ComputeKey / D3DCompile picks up any include edits the user made
    // alongside their Shader.ini change.
    ShaderCache::InvalidateIncludeMemo();
    ShadowMapDeferredLighting::InvalidateShader();
    TiledDeferredLighting::InvalidateShader();
    // Remove all connections ShaderDB <> ShaderDefDB
    g_ShaderDB.UnmatchAll();
    // STOP and destroy all file watchers BEFORE processing new INI files
    for (auto* def : g_shaderDefinitions.definitions) {
        if (def->hlslFileWatcher) {
            def->hlslFileWatcher->Stop();  // Stop background thread
            def->hlslFileWatcher.reset();  // Destroy file watcher
        }
    }
    g_iniWatchers.clear();  // Stop INI file watchers
    // At this point we have no active watchers and no connections between ShaderDB and ShaderDefDB
    g_shaderDefinitions.Clear();  // Deletes old definitions
    CustomPass::g_registry.Reset();  // Wipe customPass / customResource state for re-parse
    GpuScalar::Reset();              // Wipe gpuScalar probes alongside customPass state
    // Build a new ShaderDefDB from the INI files on disk
    if (g_shaderFolderPath.empty()) {
        g_shaderFolderPath = g_pluginPath / "ShaderEngine";
    }
    auto shaderFolders = GetSubdirectories(g_shaderFolderPath);
    for (const auto& folderPath : shaderFolders) {
        std::string folderName = folderPath.filename().string();
        LoadShaderDefinitionsFromFile(folderPath, folderName);  // Loads into g_shaderDefinitions
    }
    // Sort definitions by priority
    g_shaderDefinitions.SortByPriority();
    // Start HLSL watchers for definitions
    if (DEVELOPMENT) {
        REX::INFO("ReloadAllShaderDefinitions_Internal: Setting up development file watchers for shader definitions...");
        for (auto* def : g_shaderDefinitions.definitions) {
            if (def->active && !def->shaderFile.empty()) {
                def->hlslFileWatcher = std::make_unique<HlslFileWatcher>(def->shaderFile);
                def->hlslFileWatcher->Start();
            }
        }
        // CustomPass watchers are recreated alongside, since the previous
        // batch was torn down by Reset().
        CustomPass::g_registry.StartFileWatchers();
    }
    // Resolve hook-id triggers on the new customPass blocks now that all
    // shader definitions are known.
    CustomPass::g_registry.ResolveHookIdTriggers();
    // Reconnect ShaderDB <> ShaderDefDB based on new definitions
    RematchAllShaders_Internal();
    // Restart the precompile worker and re-queue every active def and
    // customPass. The device is already up at this point (we got here via
    // a render-thread task), so the worker pops jobs immediately without
    // the startup wait.
    if (g_precompileWorker) {
        g_precompileWorker->Start();
        {
            std::shared_lock lk(g_shaderDefinitions.mutex);
            for (auto* def : g_shaderDefinitions.definitions) {
                if (def && def->active && !def->shaderFile.empty()) {
                    g_precompileWorker->Enqueue(def->id, [def]{ CompileShader_Internal(def); });
                }
            }
        }
        CustomPass::g_registry.EnqueuePrecompileJobs();
    }
    if (DEBUGGING) {
        REX::INFO("ReloadAllShaderDefinitions_Internal: Completed hot reload of shader definitions.");
    }
}

// Message handler definition
void F4SEMessageHandler(F4SE::MessagingInterface::Message *a_message) {
    switch (a_message->type) {
        case F4SE::MessagingInterface::kPostLoad:
            REX::INFO("Received kMessage_PostLoad. Game data is now loaded!");
            break;
        case F4SE::MessagingInterface::kPostPostLoad:
            REX::INFO("Received kMessage_PostPostLoad. Game data finished loading.");
            break;
        case F4SE::MessagingInterface::kGameLoaded:
            REX::INFO("Received kMessage_GameLoaded. A save game has been loaded.");
            ShadowUpgrade::OnGameDataReady();
            ResetPlayerRadDamageTracking();
            ClearActorDrawTaggedGeometry_Internal();
            break;
        case F4SE::MessagingInterface::kGameDataReady:
            REX::INFO("Received kMessage_GameDataReady. Game data is ready.");
            // Fallout applies INIPref-backed renderer globals after plugin load.
            // Install the first-cascade override only after the live
            // fDirShadowDistance contract is populated.
            ShadowUpgrade::OnGameDataReady();
            // Get the global data handle and interfaces
            g_dataHandle = RE::TESDataHandler::GetSingleton();
            if (g_dataHandle) {
                REX::INFO("TESDataHandler singleton acquired successfully.");
            } else {
                REX::WARN("Failed to acquire TESDataHandler singleton.");
            }
            // Install graphics hooks
            if (!GFX_HOOKS_INSTALLED) {
                if (InstallGFXHooks_Internal()) {
                    GFX_HOOKS_INSTALLED = true;
                    REX::INFO("GFX hooks installed successfully on kMessage_GameDataReady.");
                }
            } else {
                REX::WARN("Failed to install GFX hooks on kMessage_GameDataReady.");
            }
            break;
        case F4SE::MessagingInterface::kPostLoadGame:
            REX::INFO("Received kMessage_PostLoadGame. A save game has been loaded.");
            ShadowUpgrade::OnGameDataReady();
            ResetPlayerRadDamageTracking();
            ClearActorDrawTaggedGeometry_Internal();
            break;
        case F4SE::MessagingInterface::kNewGame:
            REX::INFO("Received kMessage_NewGame. A new game has been started.");
            ShadowUpgrade::OnGameDataReady();
            ResetPlayerRadDamageTracking();
            ClearActorDrawTaggedGeometry_Internal();
            // Install graphics hooks
            if (!GFX_HOOKS_INSTALLED) {
                if (InstallGFXHooks_Internal()) {
                    GFX_HOOKS_INSTALLED = true;
                    REX::INFO("GFX hooks installed successfully on kMessage_NewGame.");
                }
            } else {
                REX::WARN("Failed to install GFX hooks on kMessage_NewGame.");
            }
            break;
    }
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se, {
		.log = true,
        .logName = "ShaderEngine"
	});
    // Log information
    REX::INFO("Game runtime version: {}", a_f4se->RuntimeVersion().string());
    // Get the global plugin handle and interfaces
    g_pluginHandle = a_f4se->GetPluginHandle();
    g_messaging = F4SE::GetMessagingInterface();
    g_papyrus = F4SE::GetPapyrusInterface();
    // Get the DLL handle for this plugin
    HMODULE hModule = GetThisModuleHandle();
    // Fill the global plugin path variable for use in other parts of the plugin (like config loading and shader loading)
    g_pluginPath = std::filesystem::path{ "Data\\F4SE\\Plugins" };
    // Load config
    LoadConfig(hModule);
    // Sort the shader definitions by priority (highest first)
    // So we match the definitions with then highest priority
    g_shaderDefinitions.SortByPriority();
    // Light-tracker reset. Does no work itself until DEVELOPMENT is on and
    // the user presses Numpad *; D3D resources are allocated lazily on first
    // capture, so it's safe to call this before the renderer is up.
    LightTracker::Initialize();
    // Cull-policy module: arms BSLight::TestFrustumCull to consult active
    // shader rules for an optional bound-radius scale value. Side-effect-free
    // until a Shader.ini block sets `lightCullRadiusScaleValue` AND that
    // rule's replacement compiles.
    LightCullPolicy::Initialize();
    // GPU scalar probes: per-frame CS dispatches that bridge HLSL-evaluated
    // scalars back to CPU. Side-effect-free until at least one [gpuScalar:NAME]
    // section is parsed AND its include + entry function compile cleanly.
    GpuScalar::Initialize();
    // Get the task interface
    g_taskInterface = F4SE::GetTaskInterface();
    // Allocate Trampoline size
    auto& trampoline = REL::GetTrampoline();
    // Budget covers ShaderEngine's pre-existing branch gateway hooks
    // (~210 B on OG) plus the 5-byte branch hooks for Update3DModel/Reset3D
    // (each ~19 B gateway + 14 B abs-jump thunk in this same arena) plus
    // PhaseTelemetry's DrawWorld:: per-phase hooks (14 sites × ~30 B ? 420 B),
    // plus ShadowTelemetry's direct call-site patches. 4 KB leaves headroom
    // for write_call thunks and future measurement hooks.
    trampoline.create(4096);
    // Install the shader creation hooks very early.
    InstallShaderCreationHooks_Internal();
    InstallDrawTaggingHooks_Internal();
    // The shadow-upgrade module owns the sole DeferredLightsImpl wrapper used
    // for renderer phase context. It is observe/dispatch-only unless a
    // separately gated consumer is enabled.
    ShadowUpgrade::Initialize();
    // Phase telemetry installs per-DrawWorld:: hooks to attribute wall time
    // + draw count per sub-phase under DrawWorld::Render_PreUI. Default off
    // = no logging.
#if SHADERENGINE_ENABLE_PHASE_TELEMETRY
    PhaseTelemetry::Initialize();
#endif
#if SHADERENGINE_ENABLE_SHADOW_TELEMETRY
    ShadowTelemetry::Initialize();
#endif
    // LightSorter stable-partitions the point-light array by stencil flag
    // inside ShadowUpgrade's validated DeferredLightsImpl wrapper.
    LightSorter::Initialize();
    // Get the scaleform interface
    g_scaleformInterface = F4SE::GetScaleformInterface();
    // Set the messagehandler to listen to events
    if (g_messaging && g_messaging->RegisterListener(F4SEMessageHandler, "F4SE")) {
        REX::INFO("Registered F4SE message handler.");
    } else {
        REX::WARN("Failed to register F4SE message handler.");
        return false;
    }
    return true;
}

// F4SE_PLUGIN_VERSION is defined by the auto-generated commonlibf4-plugin.cpp
// (see lib/commonlibf4/res/commonlibf4-plugin.cpp.in). The xmake.lua
// `commonlibf4.plugin` rule fills name/author/version from set_version().
// Keeping a duplicate here triggered an LNK2005 with current commonlibf4.

// --- F4SE Entry Points - MUST have C linkage for F4SE to find them ---
extern "C"
{ // This block ensures C-style (unmangled) names for the linker
    F4SE_API bool F4SEPlugin_Query(const F4SE::LoadInterface* f4se, F4SE::PluginInfo * info)
    {
        info->infoVersion = F4SE::PluginInfo::kVersion;
        info->name = Version::PROJECT.data();
        info->version = Version::MAJOR;

        return true;
    }

    F4SE_API void F4SEPlugin_Release() {
        // This is a new function for cleanup. It is called when the plugin is unloaded.
        REX::INFO("{}: Plugin released.", Version::PROJECT);
        gLog->flush();
        spdlog::drop_all();
        // Stop all file watchers
        for (auto* def : g_shaderDefinitions.definitions) {
            if (def->hlslFileWatcher) {
                def->hlslFileWatcher->Stop();
                def->hlslFileWatcher.reset();
            }
        }
        // Stop all INI watchers
        for (auto& [path, watcher] : g_iniWatchers) {
            if (watcher) {
                watcher->Stop();
                watcher.reset();
            }
        }
        // Stop the include-dir watcher (if running).
        if (g_includeDirWatcher) {
            g_includeDirWatcher->Stop();
            g_includeDirWatcher.reset();
        }
        // Stop the background precompile worker BEFORE we release any D3D11
        // resources ? a worker mid-CompileShader_Internal could otherwise
        // call CreatePixelShader on a torn-down device.
        if (g_precompileWorker) {
            g_precompileWorker->Stop();
            g_precompileWorker.reset();
        }
        // Release the light-tracker staging buffer before D3D teardown.
        LightTracker::Shutdown();
        // Disarm the cull-policy hook gate so any in-flight cull running
        // during teardown bails out of the slow path cheaply.
        LightCullPolicy::Shutdown();
        // Release GPU-scalar probe resources (CS + UAV buffer + staging ring).
        GpuScalar::Shutdown();
        ShadowMapDeferredLighting::Shutdown();
        TiledDeferredLighting::Shutdown();
        // Clear Shader resources
        if (g_customSRV)       { g_customSRV->Release();       g_customSRV = nullptr; }
        if (g_customSRVBuffer) { g_customSRVBuffer->Release(); g_customSRVBuffer = nullptr; }
        ReleaseDrawTagBuffers_Internal();
        if (g_modularFloatsSRV)       { g_modularFloatsSRV->Release();       g_modularFloatsSRV = nullptr; }
        if (g_modularFloatsSRVBuffer) { g_modularFloatsSRVBuffer->Release(); g_modularFloatsSRVBuffer = nullptr; }
        if (g_modularIntsSRV)         { g_modularIntsSRV->Release();         g_modularIntsSRV = nullptr; }
        if (g_modularIntsSRVBuffer)   { g_modularIntsSRVBuffer->Release();   g_modularIntsSRVBuffer = nullptr; }
        if (g_modularBoolsSRV)        { g_modularBoolsSRV->Release();        g_modularBoolsSRV = nullptr; }
        if (g_modularBoolsSRVBuffer)  { g_modularBoolsSRVBuffer->Release();  g_modularBoolsSRVBuffer = nullptr; }
        ClearActorDrawTaggedGeometry_Internal();
    }
}
