#include <Global.h>
#include <PCH.h>
#include <CustomPass.h>
#include <LocalLightBridge.h>
#include <RenderTargets.h>
#include <ShaderResources.h>
#include <d3d11.h>

// Helpers shared with main.cpp (defined there).
extern std::string RemoveInlineComment(const std::string& line);
extern std::pair<std::string, std::string> GetKeyValueFromLine(const std::string& line);
namespace { inline std::string RemoveAllWS(std::string s) { s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c){ return std::isspace(c); }), s.end()); return s; } }

extern std::filesystem::path g_shaderFolderPath;
// Plugin.cpp owns this atomic; declare it at namespace scope so the
// CustomPass module can read it without dragging the symbol into its own
// namespace (which would generate a CustomPass::-qualified linker symbol).
extern std::atomic<REX::W32::ID3D11PixelShader*> g_currentOriginalPixelShader;

// Forward declarations for the shader replacement / database lookups in
// Plugin.cpp. We only need to look up an existing definition's first ShaderUID
// for triggerHookId resolution.
extern ShaderDefDB g_shaderDefinitions;

namespace CustomPass {

Registry g_registry;

}  // namespace CustomPass

thread_local bool g_customPassRendering = false;

namespace CustomPass {

// --- FileWatcher --------------------------------------------------------

FileWatcher::FileWatcher(std::filesystem::path path, OnChange cb)
    : filePath(std::move(path)), onChange(std::move(cb)) {
    if (std::filesystem::exists(filePath)) {
        lastWriteTime = std::filesystem::last_write_time(filePath);
    }
}
FileWatcher::~FileWatcher() { Stop(); }

void FileWatcher::Start() {
    running = true;
    watcherThread = std::thread([this]() {
        std::unique_lock lock(stopMutex);
        while (running) {
            lock.unlock();
            try {
                if (std::filesystem::exists(filePath)) {
                    auto cur = std::filesystem::last_write_time(filePath);
                    if (cur != lastWriteTime) {
                        lastWriteTime = cur;
                        if (onChange) onChange();
                    }
                }
            } catch (...) {}
            lock.lock();
            stopCv.wait_for(lock, std::chrono::seconds(1), [this]{ return !running; });
        }
    });
}
void FileWatcher::Stop() {
    {
        std::lock_guard lock(stopMutex);
        running = false;
    }
    stopCv.notify_all();
    if (watcherThread.joinable()) watcherThread.join();
}

// --- Format & scale parsing ----------------------------------------------

namespace {

REX::W32::DXGI_FORMAT ParseFormat(const std::string& s) {
    static const std::unordered_map<std::string, REX::W32::DXGI_FORMAT> table = {
        { "R8G8B8A8_UNORM",     REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM },
        { "R8G8B8A8_UNORM_SRGB",REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM_SRGB },
        { "R10G10B10A2_UNORM",  REX::W32::DXGI_FORMAT_R10G10B10A2_UNORM },
        { "R11G11B10_FLOAT",    REX::W32::DXGI_FORMAT_R11G11B10_FLOAT },
        { "R16G16B16A16_FLOAT", REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT },
        { "R16G16_FLOAT",       REX::W32::DXGI_FORMAT_R16G16_FLOAT },
        { "R16_FLOAT",          REX::W32::DXGI_FORMAT_R16_FLOAT },
        { "R32G32B32A32_FLOAT", REX::W32::DXGI_FORMAT_R32G32B32A32_FLOAT },
        { "R32G32_FLOAT",       REX::W32::DXGI_FORMAT_R32G32_FLOAT },
        { "R32_FLOAT",          REX::W32::DXGI_FORMAT_R32_FLOAT },
        { "R8_UNORM",           REX::W32::DXGI_FORMAT_R8_UNORM },
    };
    auto it = table.find(s);
    return (it != table.end()) ? it->second : REX::W32::DXGI_FORMAT_R11G11B10_FLOAT;
}

void ParseScale(const std::string& s, ScaleMode& mode, uint32_t& div, uint32_t& w, uint32_t& h) {
    mode = ScaleMode::Screen; div = 1; w = 0; h = 0;
    if (s.empty()) return;
    if (s == "screen") { mode = ScaleMode::Screen; div = 1; return; }
    if (s.rfind("screen/", 0) == 0) {
        mode = ScaleMode::ScreenDiv;
        try { div = static_cast<uint32_t>(std::stoul(s.substr(7))); } catch (...) { div = 1; }
        if (div == 0) div = 1;
        return;
    }
    auto x = s.find('x');
    if (x != std::string::npos) {
        mode = ScaleMode::Absolute;
        try { w = static_cast<uint32_t>(std::stoul(s.substr(0, x))); } catch (...) { w = 0; }
        try { h = static_cast<uint32_t>(std::stoul(s.substr(x + 1))); } catch (...) { h = 0; }
    }
}

void ResolveScale(ScaleMode mode, uint32_t div, uint32_t absW, uint32_t absH,
                  uint32_t backW, uint32_t backH,
                  uint32_t& outW, uint32_t& outH) {
    switch (mode) {
        case ScaleMode::Screen:    outW = backW; outH = backH; break;
        case ScaleMode::ScreenDiv: outW = std::max<uint32_t>(1, backW / std::max<uint32_t>(1, div));
                                   outH = std::max<uint32_t>(1, backH / std::max<uint32_t>(1, div)); break;
        case ScaleMode::Absolute:  outW = absW ? absW : backW; outH = absH ? absH : backH; break;
    }
}

bool ParseInputBinding(const std::string& token, InputBinding& out) {
    // Format: "<slot>:<source>" where source is depth | currentRTV |
    // currentPSRV:N | customResource:NAME | gbufferRT:N
    auto colon = token.find(':');
    if (colon == std::string::npos) return false;
    try { out.slot = std::stoi(token.substr(0, colon)); } catch (...) { return false; }
    std::string source = token.substr(colon + 1);
    const std::string lowerSource = ToLower(source);

    if (lowerSource == "depth")            { out.kind = InputKind::Depth; return true; }
    if (lowerSource == "currentrtv" || lowerSource == "currentrtv0") { out.kind = InputKind::CurrentRTV; return true; }
    if (lowerSource == "gbuffernormal")    { out.kind = InputKind::GBufferNormal; return true; }
    if (lowerSource == "gbufferalbedo")    { out.kind = InputKind::GBufferAlbedo; return true; }
    if (lowerSource == "gbuffermaterial")  { out.kind = InputKind::GBufferMaterial; return true; }
    if (lowerSource == "motionvectors")    { out.kind = InputKind::MotionVectors; return true; }
    if (lowerSource == "scenehdr")         { out.kind = InputKind::SceneHDR; return true; }
    if (lowerSource.rfind("currentpsrv:", 0) == 0) {
        out.kind = InputKind::CurrentPSRV;
        try { out.sourceSlot = std::stoi(source.substr(strlen("currentPSRV:"))); } catch (...) { return false; }
        return true;
    }
    if (lowerSource.rfind("customresource:", 0) == 0) {
        out.kind = InputKind::Resource;
        out.resourceName = source.substr(strlen("customResource:"));
        return true;
    }
    if (lowerSource.rfind("gbufferrt:", 0) == 0) {
        out.kind = InputKind::GBufferRT;
        try { out.gbufferIndex = std::stoi(source.substr(strlen("gbufferRT:"))); } catch (...) { return false; }
        return true;
    }
    return false;
}

bool ParseOutputBinding(const std::string& token, OutputBinding& out) {
    auto colon = token.find(':');
    if (colon == std::string::npos) return false;
    try { out.slot = std::stoi(token.substr(0, colon)); } catch (...) { return false; }
    std::string source = token.substr(colon + 1);
    if (source.rfind("customResource:", 0) == 0) {
        out.kind = OutputKind::Resource;
        out.resourceName = source.substr(strlen("customResource:"));
        return true;
    }
    if (source.rfind("gbufferRT:", 0) == 0) {
        out.kind = OutputKind::GBufferRT;
        try { out.gbufferIndex = std::stoi(source.substr(strlen("gbufferRT:"))); } catch (...) { return false; }
        return true;
    }
    return false;
}

void ParseList(const std::string& value, std::vector<std::string>& out) {
    std::stringstream ss(value);
    std::string seg;
    while (std::getline(ss, seg, ',')) if (!seg.empty()) out.push_back(seg);
}

ThreadGroupDim ParseThreadGroupDim(const std::string& s) {
    ThreadGroupDim d{};
    if (s.rfind("screen/", 0) == 0) {
        d.mode = ScaleMode::ScreenDiv;
        try { d.value = static_cast<uint32_t>(std::stoul(s.substr(7))); } catch (...) { d.value = 1; }
    } else if (s == "screen") {
        d.mode = ScaleMode::Screen; d.value = 1;
    } else {
        d.mode = ScaleMode::Absolute;
        try { d.value = static_cast<uint32_t>(std::stoul(s)); } catch (...) { d.value = 1; }
    }
    return d;
}

}  // anonymous

// --- Resource ------------------------------------------------------------

bool Resource::EnsureAllocated(REX::W32::ID3D11Device* device,
                               uint32_t backbufferW, uint32_t backbufferH) {
    if (!device) return false;
    if (spec.renderDomain) {
        constexpr float kMinimumRenderExtent = 16.0f;
        const bool renderExtentValid =
            std::isfinite(g_customBufferData.g_RenderInfo.x) &&
            std::isfinite(g_customBufferData.g_RenderInfo.y) &&
            g_customBufferData.g_RenderInfo.x >= kMinimumRenderExtent &&
            g_customBufferData.g_RenderInfo.y >= kMinimumRenderExtent;
        if (renderExtentValid) {
            const auto renderW = static_cast<uint32_t>(
                std::round(g_customBufferData.g_RenderInfo.x));
            const auto renderH = static_cast<uint32_t>(
                std::round(g_customBufferData.g_RenderInfo.y));
            // The injected domain is renderer telemetry, not permission to
            // allocate beyond the live kMain contract. This also makes a mode
            // transition fail safely if the injected buffer and the renderer
            // briefly disagree for one frame.
            backbufferW = (std::min)(renderW, backbufferW);
            backbufferH = (std::min)(renderH, backbufferH);
        }
    }
    uint32_t targetW = 0, targetH = 0;
    ResolveScale(spec.scaleMode, spec.scaleDiv, spec.absWidth, spec.absHeight,
                 backbufferW, backbufferH, targetW, targetH);
    if (texture && targetW == width && targetH == height) return true;

    Release();
    width = targetW; height = targetH;

    REX::W32::D3D11_TEXTURE2D_DESC desc{};
    desc.width            = width;
    desc.height           = height;
    desc.mipLevels        = spec.mipLevels;
    desc.arraySize        = 1;
    desc.format           = spec.format;
    desc.sampleDesc.count = 1;
    desc.usage            = REX::W32::D3D11_USAGE_DEFAULT;
    desc.bindFlags        = REX::W32::D3D11_BIND_SHADER_RESOURCE;
    if (spec.needRtv) desc.bindFlags |= REX::W32::D3D11_BIND_RENDER_TARGET;
    if (spec.needUav) desc.bindFlags |= REX::W32::D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
    if (!REX::W32::SUCCESS(hr) || !texture) {
        REX::WARN("CustomPass::Resource[{}]: CreateTexture2D failed 0x{:08X}", spec.name, hr);
        return false;
    }
    {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.format        = spec.format;
        sd.viewDimension = REX::W32::D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.texture2D.mipLevels = spec.mipLevels;
        hr = device->CreateShaderResourceView(texture, &sd, &srv);
        if (!REX::W32::SUCCESS(hr)) {
            REX::WARN("CustomPass::Resource[{}]: CreateShaderResourceView failed 0x{:08X}", spec.name, hr);
            Release(); return false;
        }
    }
    if (spec.needRtv) {
        REX::W32::D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.format        = spec.format;
        rd.viewDimension = REX::W32::D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = device->CreateRenderTargetView(texture, &rd, &rtv);
        if (!REX::W32::SUCCESS(hr)) {
            REX::WARN("CustomPass::Resource[{}]: CreateRenderTargetView failed 0x{:08X}", spec.name, hr);
            Release(); return false;
        }
    }
    if (spec.needUav) {
        REX::W32::D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.format        = spec.format;
        ud.viewDimension = REX::W32::D3D11_UAV_DIMENSION_TEXTURE2D;
        hr = device->CreateUnorderedAccessView(texture, &ud, &uav);
        if (!REX::W32::SUCCESS(hr)) {
            REX::WARN("CustomPass::Resource[{}]: CreateUnorderedAccessView failed 0x{:08X}", spec.name, hr);
            Release(); return false;
        }
    }
    REX::INFO(
        "CustomPass::Resource[{}]: allocated {}x{} (domain={})",
        spec.name,
        width,
        height,
        spec.renderDomain ? "render" : "allocation");
    return true;
}

void Resource::Release() {
    if (uav) { uav->Release(); uav = nullptr; }
    if (rtv) { rtv->Release(); rtv = nullptr; }
    if (srv) { srv->Release(); srv = nullptr; }
    if (texture) { texture->Release(); texture = nullptr; }
    width = height = 0;
}

void Resource::SwapContents(Resource& other) {
    std::swap(texture, other.texture);
    std::swap(rtv, other.rtv);
    std::swap(srv, other.srv);
    std::swap(uav, other.uav);
    std::swap(width, other.width);
    std::swap(height, other.height);
}

// --- Pass ---------------------------------------------------------------

void Pass::Release() {
    if (hlslWatcher) { hlslWatcher->Stop(); hlslWatcher.reset(); }
    if (psShader) { psShader->Release(); psShader = nullptr; }
    if (csShader) { csShader->Release(); csShader = nullptr; }
    if (compiledBlob) { compiledBlob->Release(); compiledBlob = nullptr; }
    compileTried = false; compileFailed = false;
}

// --- Snapshot SRV cache --------------------------------------------------

REX::W32::ID3D11ShaderResourceView* SnapshotSrvCache::Get(REX::W32::ID3D11Device* device,
                                                           REX::W32::ID3D11Texture2D* texture) {
    if (!device || !texture) return nullptr;
    std::lock_guard lk(mutex);
    auto it = entries.find(texture);
    if (it != entries.end()) return it->second;

    REX::W32::D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (!(desc.bindFlags & REX::W32::D3D11_BIND_SHADER_RESOURCE)) {
        // Texture wasn't created with SRV bind — can't view it. Could fall
        // back to a copy-out path; for now log and skip.
        REX::WARN("CustomPass::SnapshotSrvCache: texture without SHADER_RESOURCE bind, skipping");
        entries[texture] = nullptr;
        return nullptr;
    }
    REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.format = desc.format;
    sd.viewDimension = REX::W32::D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.texture2D.mipLevels = desc.mipLevels ? desc.mipLevels : 1;
    REX::W32::ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = device->CreateShaderResourceView(texture, &sd, &srv);
    if (!REX::W32::SUCCESS(hr)) {
        REX::WARN("CustomPass::SnapshotSrvCache: CreateShaderResourceView failed 0x{:08X}", hr);
        entries[texture] = nullptr;
        return nullptr;
    }
    entries[texture] = srv;
    return srv;
}

void SnapshotSrvCache::Release() {
    std::lock_guard lk(mutex);
    for (auto& [tex, srv] : entries) if (srv) srv->Release();
    entries.clear();
}

// --- Fullscreen triangle VS ----------------------------------------------

namespace {
const char* kFullscreenVS = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";
REX::W32::ID3D11VertexShader* g_fsVS = nullptr;
}

REX::W32::ID3D11VertexShader* GetFullscreenTriangleVS(REX::W32::ID3D11Device* device) {
    if (g_fsVS || !device) return g_fsVS;
    ID3DBlob* blob = nullptr;
    ID3DBlob* err  = nullptr;
    HRESULT hr = D3DCompile(kFullscreenVS, strlen(kFullscreenVS), "FullscreenTriangleVS",
                            nullptr, nullptr, "main", "vs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (!REX::W32::SUCCESS(hr)) {
        if (err) { REX::WARN("CustomPass: FullscreenTriangleVS compile failed: {}", static_cast<const char*>(err->GetBufferPointer())); err->Release(); }
        return nullptr;
    }
    if (err) err->Release();
    ::g_isCreatingReplacementShader = true;
    hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_fsVS);
    ::g_isCreatingReplacementShader = false;
    blob->Release();
    if (!REX::W32::SUCCESS(hr)) {
        REX::WARN("CustomPass: CreateVertexShader for fullscreen triangle failed 0x{:08X}", hr);
        return nullptr;
    }
    return g_fsVS;
}

// --- Registry ------------------------------------------------------------

bool Registry::IsCustomSection(const std::string& sectionName) {
    return sectionName.rfind("customPass:", 0) == 0
        || sectionName.rfind("customResource:", 0) == 0;
}

void Registry::Reset() {
    std::lock_guard lk(mutex);
    drawPassCacheGeneration.fetch_add(1, std::memory_order_acq_rel);
    drawBatchCache.clear();
    hasDrawTimePasses.store(false, std::memory_order_release);
    hasGlobalResourceBindings.store(false, std::memory_order_release);
    snapshotCache.Release();
    for (auto& p : passes) p->Release();
    passes.clear();
    for (auto& r : resources) r->Release();
    resources.clear();
    uidIndex.clear();
    hookIdIndex.clear();
    defIndex.clear();
    drawDefIndex.clear();
    resourceIndex.clear();
}

void Registry::InvalidateDrawPassCache() {
    drawPassCacheGeneration.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard lk(mutex);
    drawBatchCache.clear();
    hasDrawTimePasses.store(!drawDefIndex.empty(), std::memory_order_release);
}

bool Registry::ParseSection(const std::string& sectionName,
                            std::ifstream& file,
                            const std::string& endTag,
                            const std::filesystem::path& folderPath,
                            const std::string& folderName) {
    if (sectionName.rfind("customResource:", 0) == 0) {
        return ParseResourceSection(sectionName.substr(strlen("customResource:")), file, endTag);
    }
    if (sectionName.rfind("customPass:", 0) == 0) {
        return ParsePassSection(sectionName.substr(strlen("customPass:")), file, endTag, folderPath);
    }
    return false;
}

bool Registry::ParseResourceSection(const std::string& name,
                                     std::ifstream& file,
                                     const std::string& endTag) {
    auto res = std::make_unique<Resource>();
    res->spec.name = name;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == ';') continue;
        std::string clean = RemoveAllWS(RemoveInlineComment(line));
        if (clean.empty()) continue;
        if (ToLower(clean) == ToLower(endTag)) break;
        auto [key, value] = GetKeyValueFromLine(clean);
        if (key.empty() || value.empty()) continue;
        std::string lk = ToLower(key);

        if      (lk == "format")          res->spec.format = ParseFormat(value);
        else if (lk == "scale")           ParseScale(value, res->spec.scaleMode, res->spec.scaleDiv, res->spec.absWidth, res->spec.absHeight);
        else if (lk == "domain")          res->spec.renderDomain = (ToLower(value) == "render");
        else if (lk == "miplevels")       { try { res->spec.mipLevels = static_cast<uint32_t>(std::stoul(value)); } catch (...) {} }
        else if (lk == "srvslot")         { try { res->spec.srvSlot = std::stoi(value); } catch (...) {} }
        else if (lk == "global" || lk == "globalbind") res->spec.globalBind = (ToLower(value) == "true" || value == "1");
        else if (lk == "uav")             res->spec.needUav = (ToLower(value) == "true" || value == "1");
        else if (lk == "rtv")             res->spec.needRtv = (ToLower(value) == "true" || value == "1");
        else if (lk == "clearonpresent")  res->spec.clearOnPresent = (ToLower(value) == "true" || value == "1");
        else if (lk == "clearcolor") {
            std::vector<std::string> parts; ParseList(value, parts);
            const size_t n = parts.size() < 4u ? parts.size() : 4u;
            for (size_t i = 0; i < n; ++i) {
                try { res->spec.clearColor[i] = std::stof(parts[i]); } catch (...) {}
            }
        }
        else if (lk == "copyfrom")        res->spec.copyFrom = value;
        else if (lk == "copyat")          res->spec.copyAt = value;
        else if (lk == "persistent")      res->spec.persistent = (ToLower(value) == "true" || value == "1");
        else if (lk == "pingpongwith")    res->spec.pingpongWith = value;
    }

    std::lock_guard lk(mutex);
    Resource* raw = res.get();
    resourceIndex[name] = raw;
    if (raw->spec.globalBind && raw->spec.srvSlot >= 0) {
        hasGlobalResourceBindings.store(true, std::memory_order_release);
    }
    resources.push_back(std::move(res));
    REX::INFO("CustomPass: registered customResource '{}' (slot t{}, global={}, domain={})",
        name,
        raw->spec.srvSlot,
        raw->spec.globalBind ? "true" : "false",
        raw->spec.renderDomain ? "render" : "allocation");
    return true;
}

bool Registry::ParsePassSection(const std::string& name,
                                 std::ifstream& file,
                                 const std::string& endTag,
                                 const std::filesystem::path& folderPath) {
    auto pass = std::make_unique<Pass>();
    pass->spec.name = name;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == ';') continue;
        std::string clean = RemoveAllWS(RemoveInlineComment(line));
        if (clean.empty()) continue;
        if (ToLower(clean) == ToLower(endTag)) break;
        auto [key, value] = GetKeyValueFromLine(clean);
        if (key.empty() || value.empty()) continue;
        std::string lk = ToLower(key);

        if      (lk == "active")        pass->spec.active = (ToLower(value) == "true" || value == "1");
        else if (lk == "activewhen")    pass->spec.activeWhen = value;
        else if (lk == "priority")      { try { pass->spec.priority = std::stoi(value); } catch (...) {} }
        else if (lk == "type")          pass->spec.type = (ToLower(value) == "cs") ? PassType::Compute : PassType::Pixel;
        else if (lk == "shader") {
            std::filesystem::path p = folderPath / value;
            if (std::filesystem::exists(p)) pass->spec.shaderFile = p;
            else REX::WARN("CustomPass[{}]: shader file not found: {}", name, p.string());
        }
        else if (lk == "entry")         pass->spec.entry = value;
        else if (lk == "trigger") {
            // beforeShaderUID:UID    raw-UID PSSetShader-time (collision-prone)
            // beforeHookId:ID        PSSetShader-time, matched-def (stale state)
            // beforeDrawForHook:ID   Draw-time, matched-def (FRESH state — preferred)
            // atPresent              fire each frame in Present
            if (value.rfind("beforeShaderUID:", 0) == 0) {
                pass->spec.trigger = TriggerKind::BeforeShaderUID;
                pass->spec.triggerUID = value.substr(strlen("beforeShaderUID:"));
            } else if (value.rfind("beforeHookId:", 0) == 0) {
                pass->spec.trigger = TriggerKind::BeforeHookId;
                pass->spec.triggerHookId = value.substr(strlen("beforeHookId:"));
            } else if (value.rfind("beforeDrawForHook:", 0) == 0) {
                // Reuse BeforeHookId as the unresolved state; the resolver
                // promotes it to BeforeDrawForMatchedDef instead of
                // BeforeMatchedDefinition based on the spec.atDrawTime hint.
                pass->spec.trigger = TriggerKind::BeforeHookId;
                pass->spec.triggerHookId = value.substr(strlen("beforeDrawForHook:"));
                pass->spec.atDrawTime = true;
            } else if (value == "atPresent") {
                pass->spec.trigger = TriggerKind::AtPresent;
            }
        }
        else if (lk == "triggershaderuid")     { pass->spec.trigger = TriggerKind::BeforeShaderUID; pass->spec.triggerUID = value; }
        else if (lk == "triggerhookid")        { pass->spec.trigger = TriggerKind::BeforeHookId;    pass->spec.triggerHookId = value; }
        else if (lk == "triggerdrawforhookid") { pass->spec.trigger = TriggerKind::BeforeHookId;    pass->spec.triggerHookId = value; pass->spec.atDrawTime = true; }
        else if (lk == "triggeratpresent")     { if (ToLower(value) == "true" || value == "1") pass->spec.trigger = TriggerKind::AtPresent; }
        else if (lk == "onceperframe")  pass->spec.oncePerFrame = (ToLower(value) == "true" || value == "1");
        else if (lk == "input") {
            std::vector<std::string> parts; ParseList(value, parts);
            for (auto& tok : parts) { InputBinding b; if (ParseInputBinding(tok, b)) pass->spec.inputs.push_back(b); }
        }
        else if (lk == "output" || lk == "uav") {
            std::vector<std::string> parts; ParseList(value, parts);
            for (auto& tok : parts) { OutputBinding b; if (ParseOutputBinding(tok, b)) pass->spec.outputs.push_back(b); }
        }
        else if (lk == "viewport")     {
            uint32_t dummyW = 0, dummyH = 0;  // viewport scale ignores absolute width/height
            ParseScale(value, pass->spec.viewportMode, pass->spec.viewportDiv, dummyW, dummyH);
        }
        else if (lk == "clearonfire")  pass->spec.clearOnFire = (ToLower(value) == "true" || value == "1");
        else if (lk == "depthtest")    pass->spec.depthTest = (ToLower(value) == "true" || value == "1");
        else if (lk == "blend") {
            std::string v = ToLower(value);
            pass->spec.blend = (v == "additive")    ? BlendMode::Additive
                             : (v == "premulalpha") ? BlendMode::PremulAlpha
                             : (v == "multiply")    ? BlendMode::Multiply
                             :                        BlendMode::Opaque;
        }
        else if (lk == "log")          pass->spec.log = (ToLower(value) == "true" || value == "1");
        else if (lk == "threadgroups") {
            std::vector<std::string> parts; ParseList(value, parts);
            const size_t n = parts.size() < 3u ? parts.size() : 3u;
            for (size_t i = 0; i < n; ++i)
                pass->spec.threadGroups[i] = ParseThreadGroupDim(parts[i]);
        }
    }

    if (!pass->spec.active) {
        REX::INFO("CustomPass: '{}' inactive — registered but disabled", name);
    }

    std::lock_guard lk(mutex);
    Pass* raw = pass.get();
    if (raw->spec.trigger == TriggerKind::BeforeShaderUID && !raw->spec.triggerUID.empty()) {
        uidIndex.emplace(raw->spec.triggerUID, raw);
    } else if (raw->spec.trigger == TriggerKind::BeforeHookId && !raw->spec.triggerHookId.empty()) {
        hookIdIndex.emplace(raw->spec.triggerHookId, raw);
    }
    passes.push_back(std::move(pass));
    REX::INFO("CustomPass: registered customPass '{}'", name);
    return true;
}

Resource* Registry::FindResource(const std::string& name) {
    auto it = resourceIndex.find(name);
    return (it != resourceIndex.end()) ? it->second : nullptr;
}

void Registry::ResolveHookIdTriggers() {
    std::lock_guard lk(mutex);
    drawPassCacheGeneration.fetch_add(1, std::memory_order_acq_rel);
    drawBatchCache.clear();
    for (auto& [id, pass] : hookIdIndex) {
        // Bind to the actual ShaderDefinition* by id. The atDrawTime flag
        // (set during parsing for `beforeDrawForHook:`) chooses whether the
        // pass goes into the PSSetShader-time or Draw-time index.
        std::shared_lock dlk(g_shaderDefinitions.mutex);
        for (auto* def : g_shaderDefinitions.definitions) {
            if (def && def->id == id) {
                if (pass->spec.atDrawTime) {
                    pass->spec.trigger = TriggerKind::BeforeDrawForMatchedDef;
                    drawDefIndex.emplace(def, pass);
                    REX::INFO("CustomPass: resolved beforeDrawForHook:{} -> def {} (UID hint: {})",
                        id, static_cast<void*>(def),
                        def->shaderUID.empty() ? "(none)" : def->shaderUID.front());
                } else {
                    pass->spec.trigger = TriggerKind::BeforeMatchedDefinition;
                    defIndex.emplace(def, pass);
                    REX::INFO("CustomPass: resolved beforeHookId:{} -> def {} (UID hint: {})",
                        id, static_cast<void*>(def),
                        def->shaderUID.empty() ? "(none)" : def->shaderUID.front());
                }
                break;
            }
        }
    }
    hasDrawTimePasses.store(!drawDefIndex.empty(), std::memory_order_release);
}

bool Registry::EnsureCompiled(Pass& pass) {
    // Cheap pre-check off the lock so the render-thread fast path doesn't
    // touch the mutex when the pass is already compiled.
    if (pass.psShader || pass.csShader) return true;
    if (pass.compileFailed) return false;

    if (!pass.compileMutex) pass.compileMutex = std::make_unique<std::mutex>();
    std::lock_guard compileLock(*pass.compileMutex);
    // Re-test under the lock — a concurrent compile may have just finished.
    if (pass.psShader || pass.csShader) return true;
    if (pass.compileFailed) return false;
    if (pass.compileTried) return false;
    pass.compileTried = true;

    if (pass.spec.shaderFile.empty()) { pass.compileFailed = true; return false; }
    if (!g_rendererData || !g_rendererData->device) {
        // Don't latch compileFailed for "device not ready" — the worker may
        // be the one calling, and the render thread will retry.
        pass.compileTried = false;
        return false;
    }

    std::ifstream f(pass.spec.shaderFile, std::ios::binary);
    if (!f.good()) { pass.compileFailed = true; return false; }
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    // Reuse the same injected header as replacement shaders. This means the
    // pass shader can use GFXInjected, ReconstructWorldPos, etc. exactly the
    // same way as a hook shader.
    std::string source = GetCommonShaderHeaderHLSLTop();
    source += GetCommonShaderHeaderHLSLBottom();
    // Inject named accessors for modular shader values.
    for (auto* sv : g_shaderSettings.GetFloatShaderValues())
        source += std::format("#define {} GFXModularFloats[{}]{}\n", sv->id, sv->bufferIndex / 4,
            std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sv->bufferIndex % 4]);
    for (auto* sv : g_shaderSettings.GetIntShaderValues())
        source += std::format("#define {} GFXModularInts[{}]{}\n", sv->id, sv->bufferIndex / 4,
            std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sv->bufferIndex % 4]);
    for (auto* sv : g_shaderSettings.GetBoolShaderValues())
        source += std::format("#define {} (GFXModularBools[{}]{} != 0)\n", sv->id, sv->bufferIndex / 4,
            std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sv->bufferIndex % 4]);
    source += "\n";
    source += body;

    const char* profile = (pass.spec.type == PassType::Compute) ? "cs_5_0" : "ps_5_0";
    constexpr uint32_t kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const std::string cacheKey = ShaderCache::ComputeKey({
        .assembledSource = source,
        .profile         = profile,
        .entry           = pass.spec.entry,
        .flags           = kCompileFlags,
    });
    if (ShaderCache::TryLoad(cacheKey, &pass.compiledBlob)) {
        REX::INFO("CustomPass[{}]: cache HIT ({} bytes)", pass.spec.name, pass.compiledBlob->GetBufferSize());
    } else {
        ID3DBlob* errBlob = nullptr;
        auto* includer = new ShaderIncludeHandler();
        HRESULT hr = D3DCompile(source.c_str(), source.size(), pass.spec.name.c_str(),
                                nullptr, includer, pass.spec.entry.c_str(), profile,
                                kCompileFlags, 0, &pass.compiledBlob, &errBlob);
        delete includer;
        if (!REX::W32::SUCCESS(hr)) {
            if (errBlob) {
                REX::WARN("CustomPass[{}]: compile failed: {}", pass.spec.name, static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            }
            pass.compileFailed = true;
            return false;
        }
        if (errBlob) errBlob->Release();
        ShaderCache::Store(cacheKey, pass.compiledBlob);
        REX::INFO("CustomPass[{}]: compiled successfully ({} bytes)", pass.spec.name, pass.compiledBlob->GetBufferSize());
    }
    HRESULT hr = S_OK;

    ::g_isCreatingReplacementShader = true;
    if (pass.spec.type == PassType::Compute) {
        hr = g_rendererData->device->CreateComputeShader(
            pass.compiledBlob->GetBufferPointer(),
            pass.compiledBlob->GetBufferSize(),
            nullptr, &pass.csShader);
    } else {
        hr = g_rendererData->device->CreatePixelShader(
            pass.compiledBlob->GetBufferPointer(),
            pass.compiledBlob->GetBufferSize(),
            nullptr, &pass.psShader);
    }
    ::g_isCreatingReplacementShader = false;
    if (!REX::W32::SUCCESS(hr)) {
        REX::WARN("CustomPass[{}]: shader object creation failed 0x{:08X}", pass.spec.name, hr);
        pass.compileFailed = true;
        return false;
    }
    return true;
}

bool Registry::EnsurePassResources(Pass& pass) {
    if (!g_rendererData || !g_rendererData->device ||
        !g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture) return false;
    REX::W32::D3D11_TEXTURE2D_DESC bd{};
    g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture->GetDesc(&bd);
    bool ok = true;
    for (auto& res : resources) {
        if (!res->EnsureAllocated(g_rendererData->device, bd.width, bd.height)) ok = false;
    }
    // Resolve pingpong partner pointers (idempotent).
    for (auto& res : resources) {
        if (res->pingpongPartner) continue;
        if (res->spec.pingpongWith.empty()) continue;
        if (auto* p = FindResource(res->spec.pingpongWith)) {
            res->pingpongPartner = p;
            p->pingpongPartner = res.get();
        }
    }
    return ok;
}

// --- Render-target identification ----------------------------------------

namespace {
// Walk renderTargets[] / depthStencilTargets[] looking for a texture that
// matches the given resource. Returns a human-readable string identifying
// which engine slot it came from. Used by the per-pass trigger-time logger
// so users can verify which render target the engine has bound to which PS
// SRV slot when the pass fires.
std::string IdentifyRenderTarget(REX::W32::ID3D11Resource* resource) {
    if (!resource || !g_rendererData) return "(null)";

    REX::W32::ID3D11Texture2D* tex = nullptr;
    if (FAILED(resource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                         reinterpret_cast<void**>(&tex))) || !tex) {
        return "(non-2D)";
    }

    std::string result;
    for (int i = 0; i < RT::idx(RT::Color::kCount); ++i) {
        const auto& rt = g_rendererData->renderTargets[i];
        if (rt.texture == tex || rt.copyTexture == tex) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "renderTargets[%d]%s", i,
                          rt.copyTexture == tex ? ".copy" : "");
            result = buf;
            break;
        }
    }
    if (result.empty()) {
        for (int i = 0; i < RT::idx(RT::Depth::kCount); ++i) {
            const auto& dt = g_rendererData->depthStencilTargets[i];
            if (dt.texture == tex) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "depthStencilTargets[%d]", i);
                result = buf;
                break;
            }
        }
    }
    if (result.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%p", static_cast<void*>(tex));
        result = "(unknown:";
        result += buf;
        result += ")";
    }
    tex->Release();
    return result;
}

// Log the engine's bound state at trigger time. Once-per-session per pass
// (rate-limited via fire count) so the F4SE log doesn't drown.
void LogEngineBindings(const Pass& pass, REX::W32::ID3D11DeviceContext* ctx, uint64_t fireCount) {
    if (fireCount != 1) return;  // only on first fire
    if (!ctx) return;

    REX::W32::ID3D11RenderTargetView* rtvs[8] = {};
    REX::W32::ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(8, rtvs, &dsv);

    REX::INFO("CustomPass[{}]: engine bindings at trigger time:", pass.spec.name);
    for (int i = 0; i < 8; ++i) {
        if (!rtvs[i]) continue;
        REX::W32::ID3D11Resource* res = nullptr;
        rtvs[i]->GetResource(&res);
        REX::INFO("  OM RTV{}: {}", i, IdentifyRenderTarget(res));
        if (res) res->Release();
        rtvs[i]->Release();
    }
    if (dsv) {
        REX::W32::ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        REX::INFO("  OM DSV : {}", IdentifyRenderTarget(res));
        if (res) res->Release();
        dsv->Release();
    }

    REX::W32::ID3D11ShaderResourceView* srvs[16] = {};
    ctx->PSGetShaderResources(0, 16, srvs);
    for (int i = 0; i < 16; ++i) {
        if (!srvs[i]) continue;
        REX::W32::ID3D11Resource* res = nullptr;
        srvs[i]->GetResource(&res);
        REX::INFO("  PS SRV t{}: {}", i, IdentifyRenderTarget(res));
        if (res) res->Release();
        srvs[i]->Release();
    }
}
}  // anonymous

// --- State save/restore ---------------------------------------------------
// NOTE: SavedState lives at CustomPass-namespace scope (NOT anonymous) so
// the header can forward-declare it for Registry::FireBatch /
// FirePassWithSaved. The rest of the file-local helpers (PassStateCache,
// EnsureSamplers, ...) remain in the anonymous namespace below.

struct SavedState {
    REX::W32::ID3D11RenderTargetView*       rtvs[8] = {};
    REX::W32::ID3D11DepthStencilView*       dsv = nullptr;
    REX::W32::D3D11_VIEWPORT                viewports[8] = {};
    UINT                                    numViewports = 0;
    REX::W32::ID3D11RasterizerState*        rs = nullptr;
    REX::W32::ID3D11BlendState*             bs = nullptr;
    float                                   blendFactor[4] = { 1, 1, 1, 1 };
    UINT                                    sampleMask = 0xffffffff;
    REX::W32::ID3D11DepthStencilState*      dss = nullptr;
    UINT                                    stencilRef = 0;
    REX::W32::ID3D11VertexShader*           vs = nullptr;
    REX::W32::ID3D11PixelShader*            ps = nullptr;
    REX::W32::ID3D11ComputeShader*          cs = nullptr;
    REX::W32::ID3D11InputLayout*            ia = nullptr;
    REX::W32::D3D11_PRIMITIVE_TOPOLOGY      topo = REX::W32::D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    REX::W32::ID3D11Buffer*                 indexBuf = nullptr;
    REX::W32::DXGI_FORMAT                   indexFormat = REX::W32::DXGI_FORMAT_UNKNOWN;
    UINT                                    indexOffset = 0;
    static constexpr UINT                   kSrvCount = 128;
    REX::W32::ID3D11ShaderResourceView*     psSrvs[kSrvCount] = {};
    REX::W32::ID3D11ShaderResourceView*     csSrvs[kSrvCount] = {};
    static constexpr UINT                   kSamplerCount = 16;
    REX::W32::ID3D11SamplerState*           psSamplers[kSamplerCount] = {};
    REX::W32::ID3D11SamplerState*           csSamplers[kSamplerCount] = {};
    static constexpr UINT                   kUavCount = 8;
    REX::W32::ID3D11UnorderedAccessView*    csUavs[kUavCount] = {};

    void Capture(REX::W32::ID3D11DeviceContext* ctx) {
        ctx->OMGetRenderTargets(8, rtvs, &dsv);
        numViewports = 8;
        ctx->RSGetViewports(&numViewports, viewports);
        ctx->RSGetState(&rs);
        ctx->OMGetBlendState(&bs, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&dss, &stencilRef);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->CSGetShader(&cs, nullptr, nullptr);
        ctx->IAGetInputLayout(&ia);
        ctx->IAGetPrimitiveTopology(&topo);
        ctx->IAGetIndexBuffer(&indexBuf, &indexFormat, &indexOffset);
        ctx->PSGetShaderResources(0, kSrvCount, psSrvs);
        ctx->CSGetShaderResources(0, kSrvCount, csSrvs);
        ctx->PSGetSamplers(0, kSamplerCount, psSamplers);
        ctx->CSGetSamplers(0, kSamplerCount, csSamplers);
        ctx->CSGetUnorderedAccessViews(0, kUavCount, csUavs);
    }
    void Restore(REX::W32::ID3D11DeviceContext* ctx) {
        ctx->OMSetRenderTargets(8, rtvs, dsv);
        ctx->RSSetViewports(numViewports, viewports);
        ctx->RSSetState(rs);
        ctx->OMSetBlendState(bs, blendFactor, sampleMask);
        ctx->OMSetDepthStencilState(dss, stencilRef);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->IASetInputLayout(ia);
        ctx->IASetPrimitiveTopology(topo);
        ctx->IASetIndexBuffer(indexBuf, indexFormat, indexOffset);
        ctx->PSSetShaderResources(0, kSrvCount, psSrvs);
        ctx->CSSetShaderResources(0, kSrvCount, csSrvs);
        ctx->PSSetSamplers(0, kSamplerCount, psSamplers);
        ctx->CSSetSamplers(0, kSamplerCount, csSamplers);
        UINT initial[kUavCount] = { 0, 0, 0, 0 };
        ctx->CSSetUnorderedAccessViews(0, kUavCount, csUavs, initial);

        for (UINT i = 0; i < 8; ++i) if (rtvs[i]) rtvs[i]->Release();
        if (dsv) dsv->Release();
        if (rs) rs->Release();
        if (bs) bs->Release();
        if (dss) dss->Release();
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (cs) cs->Release();
        if (ia) ia->Release();
        if (indexBuf) indexBuf->Release();
        for (auto* s : psSrvs) if (s) s->Release();
        for (auto* s : csSrvs) if (s) s->Release();
        for (auto* s : psSamplers) if (s) s->Release();
        for (auto* s : csSamplers) if (s) s->Release();
        for (auto* u : csUavs) if (u) u->Release();
    }
};

namespace {
// Lightweight state cache for blend/depth/raster states used by passes.
struct PassStateCache {
    REX::W32::ID3D11BlendState*             opaqueBlend = nullptr;
    REX::W32::ID3D11BlendState*             additiveBlend = nullptr;
    REX::W32::ID3D11BlendState*             premulAlphaBlend = nullptr;
    REX::W32::ID3D11BlendState*             multiplyBlend = nullptr;
    REX::W32::ID3D11DepthStencilState*      noDepth = nullptr;
    REX::W32::ID3D11RasterizerState*        passRaster = nullptr;

    REX::W32::ID3D11BlendState* GetBlend(REX::W32::ID3D11Device* dev, BlendMode mode) {
        if (mode == BlendMode::Additive) {
            if (!additiveBlend) {
                REX::W32::D3D11_BLEND_DESC d{};
                d.renderTarget[0].blendEnable = true;
                d.renderTarget[0].srcBlend = REX::W32::D3D11_BLEND_ONE;
                d.renderTarget[0].destBlend = REX::W32::D3D11_BLEND_ONE;
                d.renderTarget[0].blendOp = REX::W32::D3D11_BLEND_OP_ADD;
                d.renderTarget[0].srcBlendAlpha = REX::W32::D3D11_BLEND_ONE;
                d.renderTarget[0].destBlendAlpha = REX::W32::D3D11_BLEND_ONE;
                d.renderTarget[0].blendOpAlpha = REX::W32::D3D11_BLEND_OP_ADD;
                d.renderTarget[0].renderTargetWriteMask = 0x0F;
                dev->CreateBlendState(&d, &additiveBlend);
            }
            return additiveBlend;
        }
        if (mode == BlendMode::PremulAlpha) {
            if (!premulAlphaBlend) {
                REX::W32::D3D11_BLEND_DESC d{};
                d.renderTarget[0].blendEnable = true;
                // dst = src.rgb + dst.rgb * (1 - src.a)
                // Shader emits premultiplied src; useful for compositing where
                // src.a is the coverage/strength of the overlay.
                d.renderTarget[0].srcBlend       = REX::W32::D3D11_BLEND_ONE;
                d.renderTarget[0].destBlend      = REX::W32::D3D11_BLEND_INV_SRC_ALPHA;
                d.renderTarget[0].blendOp        = REX::W32::D3D11_BLEND_OP_ADD;
                d.renderTarget[0].srcBlendAlpha  = REX::W32::D3D11_BLEND_ONE;
                d.renderTarget[0].destBlendAlpha = REX::W32::D3D11_BLEND_INV_SRC_ALPHA;
                d.renderTarget[0].blendOpAlpha   = REX::W32::D3D11_BLEND_OP_ADD;
                d.renderTarget[0].renderTargetWriteMask = 0x0F;
                dev->CreateBlendState(&d, &premulAlphaBlend);
            }
            return premulAlphaBlend;
        }
        if (mode == BlendMode::Multiply) {
            if (!multiplyBlend) {
                REX::W32::D3D11_BLEND_DESC d{};
                d.renderTarget[0].blendEnable = true;
                // dst = src.rgb * dst.rgb. D3D11 has no MUL blend op; encode
                // it as ADD with srcBlend=DEST_COLOR and destBlend=ZERO so the
                // result becomes src*dst + dst*0.
                d.renderTarget[0].srcBlend       = REX::W32::D3D11_BLEND_DEST_COLOR;
                d.renderTarget[0].destBlend      = REX::W32::D3D11_BLEND_ZERO;
                d.renderTarget[0].blendOp        = REX::W32::D3D11_BLEND_OP_ADD;
                d.renderTarget[0].srcBlendAlpha  = REX::W32::D3D11_BLEND_ONE;
                d.renderTarget[0].destBlendAlpha = REX::W32::D3D11_BLEND_ZERO;
                d.renderTarget[0].blendOpAlpha   = REX::W32::D3D11_BLEND_OP_ADD;
                d.renderTarget[0].renderTargetWriteMask = 0x0F;
                dev->CreateBlendState(&d, &multiplyBlend);
            }
            return multiplyBlend;
        }
        if (!opaqueBlend) {
            REX::W32::D3D11_BLEND_DESC d{};
            d.renderTarget[0].renderTargetWriteMask = 0x0F;
            dev->CreateBlendState(&d, &opaqueBlend);
        }
        return opaqueBlend;
    }
    REX::W32::ID3D11DepthStencilState* GetNoDepth(REX::W32::ID3D11Device* dev) {
        if (!noDepth) {
            REX::W32::D3D11_DEPTH_STENCIL_DESC d{};
            d.depthEnable = false;
            d.stencilEnable = false;
            dev->CreateDepthStencilState(&d, &noDepth);
        }
        return noDepth;
    }
    REX::W32::ID3D11RasterizerState* GetRaster(REX::W32::ID3D11Device* dev) {
        if (!passRaster) {
            REX::W32::D3D11_RASTERIZER_DESC d{};
            d.fillMode = REX::W32::D3D11_FILL_SOLID;
            d.cullMode = REX::W32::D3D11_CULL_NONE;
            d.depthClipEnable = false;
            dev->CreateRasterizerState(&d, &passRaster);
        }
        return passRaster;
    }
} g_stateCache;

// Default sampler (linear/clamp) bound on s0 for pass shaders.
REX::W32::ID3D11SamplerState* g_passSamplerLinear = nullptr;
REX::W32::ID3D11SamplerState* g_passSamplerPoint = nullptr;
void EnsureSamplers(REX::W32::ID3D11Device* dev) {
    if (!g_passSamplerLinear) {
        REX::W32::D3D11_SAMPLER_DESC d{};
        d.filter = REX::W32::D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.addressU = d.addressV = d.addressW = REX::W32::D3D11_TEXTURE_ADDRESS_CLAMP;
        d.maxLOD = (std::numeric_limits<float>::max)();
        dev->CreateSamplerState(&d, &g_passSamplerLinear);
    }
    if (!g_passSamplerPoint) {
        REX::W32::D3D11_SAMPLER_DESC d{};
        d.filter = REX::W32::D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.addressU = d.addressV = d.addressW = REX::W32::D3D11_TEXTURE_ADDRESS_CLAMP;
        d.maxLOD = (std::numeric_limits<float>::max)();
        dev->CreateSamplerState(&d, &g_passSamplerPoint);
    }
}
}  // anonymous

void Registry::FirePass(REX::W32::ID3D11DeviceContext* context, Pass& pass) {
    if (!context) return;
    const bool wasCustomPassRendering = ::g_customPassRendering;
    ::g_customPassRendering = true;
    SavedState saved; saved.Capture(context);
    FirePassWithSaved(context, pass, saved);
    saved.Restore(context);
    ::g_customPassRendering = wasCustomPassRendering;
}

bool Registry::FireBatch(REX::W32::ID3D11DeviceContext* context, std::vector<Pass*>& matches) {
    if (!context || matches.empty()) return false;
    // Priority-stable sort: lower number fires first. Matches the engine's
    // priority convention used elsewhere.
    std::stable_sort(matches.begin(), matches.end(),
        [](Pass* a, Pass* b) { return a->spec.priority < b->spec.priority; });
    return FireSortedBatch(context, matches);
}

bool Registry::FireSortedBatch(REX::W32::ID3D11DeviceContext* context, const std::vector<Pass*>& matches) {
    if (!context || matches.empty()) return false;
    // Single capture before the chain; single restore after. Per-pass
    // FirePassWithSaved calls do NOT touch capture/restore. This collapses
    // up to N (chain-length) save/restore cycles into one, which dominates
    // CPU overhead when many passes share a trigger (e.g. SSAO + SSRTGI +
    // fake skin bloom firing at beforeDrawForHook:visualTonemap).
    const bool wasCustomPassRendering = ::g_customPassRendering;
    ::g_customPassRendering = true;
    SavedState saved; saved.Capture(context);
    bool firedAny = false;
    for (auto* p : matches) {
        if (!p) continue;
        if (!p->spec.active) continue;
        firedAny = FirePassWithSaved(context, *p, saved) || firedAny;
    }
    saved.Restore(context);
    ::g_customPassRendering = wasCustomPassRendering;
    return firedAny;
}

// Evaluate the optional `activeWhen` runtime gate. Returns true (fire)
// when the spec is empty, the bool is true, or the spec is "!id" and the
// bool is false. The resolved ShaderValue* is cached in the pass on first
// call so subsequent fires skip the linear scan.
static bool EvaluateActiveWhen(Pass& pass) {
    const std::string& spec = pass.spec.activeWhen;
    if (spec.empty()) return true;

    if (!pass.activeWhenChecked) {
        bool negate = !spec.empty() && spec[0] == '!';
        const std::string id = negate ? spec.substr(1) : spec;
        ShaderValue* resolved = nullptr;
        for (auto* sv : g_shaderSettings.GetBoolShaderValues()) {
            if (sv && sv->id == id) { resolved = sv; break; }
        }
        if (!resolved) {
            REX::WARN("CustomPass[{}]: activeWhen='{}' did not resolve to a Values.ini bool — pass will fire as if no gate were set",
                pass.spec.name, spec);
        }
        pass.activeWhenResolved = resolved;
        pass.activeWhenNegated  = negate;
        pass.activeWhenChecked  = true;
    }

    auto* sv = static_cast<ShaderValue*>(pass.activeWhenResolved);
    if (!sv) return true;  // fire-open on unknown id
    return pass.activeWhenNegated ? !sv->current.b : sv->current.b;
}

bool Registry::FirePassWithSaved(REX::W32::ID3D11DeviceContext* context, Pass& pass, SavedState& saved) {
    if (!context || !g_rendererData || !g_rendererData->device) return false;
    if (!pass.spec.active) return false;
    if (!EvaluateActiveWhen(pass)) return false;

    // Hot-reload: if the watcher thread saw a disk change, drop compiled
    // state on this (main render) thread before EnsureCompiled re-builds.
    if (pass.reloadRequested.exchange(false, std::memory_order_acq_rel)) {
        if (pass.psShader)     { pass.psShader->Release();     pass.psShader = nullptr; }
        if (pass.csShader)     { pass.csShader->Release();     pass.csShader = nullptr; }
        if (pass.compiledBlob) { pass.compiledBlob->Release(); pass.compiledBlob = nullptr; }
        pass.compileTried = false;
        pass.compileFailed = false;
    }

    if (!EnsureCompiled(pass)) return false;
    if (!EnsurePassResources(pass)) return false;

    // Per-frame gating
    if (pass.spec.oncePerFrame) {
        uint32_t prev = pass.lastFiredFrame.load(std::memory_order_acquire);
        if (prev == currentFrame) return false;
    }

    auto* device = g_rendererData->device;
    EnsureSamplers(device);

    // Diagnostic: dump engine bindings on the first fire when log=true.
    // Reads `saved` (captured by caller before any pass in the batch fired)
    // so the dump reflects engine state, not whatever a previous pass in
    // the same batch left behind.
    if (pass.spec.log) {
        const uint64_t fc = pass.totalFireCount.load(std::memory_order_relaxed);
        LogEngineBindings(pass, context, fc + 1);
    }

    // Resolve inputs
    std::vector<REX::W32::ID3D11ShaderResourceView*> srvBindings;
    int maxInputSlot = -1;
    for (auto& in : pass.spec.inputs) if (in.slot > maxInputSlot) maxInputSlot = in.slot;
    if (maxInputSlot >= 0) srvBindings.resize(maxInputSlot + 1, nullptr);

    for (auto& in : pass.spec.inputs) {
        REX::W32::ID3D11ShaderResourceView* s = nullptr;
        switch (in.kind) {
            case InputKind::Depth:
                g_depthSRV = ShaderResources::GetDepthBufferSRV_Internal();
                s = g_depthSRV;
                break;
            case InputKind::CurrentRTV: {
                // Use the snapshot we captured BEFORE Restore (saved.rtvs[0]).
                // The render target's underlying resource is the engine's HDR
                // scene texture at this point in the frame; we view it as a
                // shader-readable SRV so the pass can sample current scene color.
                if (saved.rtvs[0]) {
                    REX::W32::ID3D11Resource* res = nullptr;
                    saved.rtvs[0]->GetResource(&res);
                    if (res) {
                        REX::W32::ID3D11Texture2D* tex = nullptr;
                        res->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                            reinterpret_cast<void**>(&tex));
                        if (tex) { s = snapshotCache.Get(device, tex); tex->Release(); }
                        res->Release();
                    }
                }
                break;
            }
            case InputKind::CurrentPSRV: {
                if (in.sourceSlot >= 0 && in.sourceSlot < (int)SavedState::kSrvCount) {
                    s = saved.psSrvs[in.sourceSlot];
                }
                break;
            }
            case InputKind::Resource: {
                if (auto* r = FindResource(in.resourceName)) s = r->srv;
                break;
            }
            case InputKind::GBufferRT: {
                if (in.gbufferIndex >= 0 && in.gbufferIndex < 101) {
                    s = g_rendererData->renderTargets[in.gbufferIndex].srView;
                }
                break;
            }
            case InputKind::GBufferNormal: {
                // Configurable global index, see Global.h. Stays nullptr if
                // disabled — the consuming shader is expected to fall back
                // (typical pattern: depth-derivative normal reconstruction).
                if (NORMAL_BUFFER_INDEX >= 0 && NORMAL_BUFFER_INDEX < 101) {
                    s = g_rendererData->renderTargets[NORMAL_BUFFER_INDEX].srView;
                }
                break;
            }
            case InputKind::GBufferAlbedo:
                s = g_rendererData->renderTargets[RT::idx(RT::Color::kGbufferAlbedo)].srView;
                break;
            case InputKind::GBufferMaterial:
                s = g_rendererData->renderTargets[RT::idx(RT::Color::kGbufferMaterial)].srView;
                break;
            case InputKind::MotionVectors:
                s = g_rendererData->renderTargets[RT::idx(RT::Color::kMotionVectors)].srView;
                break;
            case InputKind::SceneHDR:
                s = g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].srView;
                break;
            default: break;
        }
        if (in.slot >= 0 && in.slot < (int)srvBindings.size()) srvBindings[in.slot] = s;
    }

    // Resolve outputs
    std::vector<REX::W32::ID3D11RenderTargetView*> rtvBindings;
    std::vector<REX::W32::ID3D11UnorderedAccessView*> uavBindings;
    int maxRtvSlot = -1, maxUavSlot = -1;
    for (auto& out : pass.spec.outputs) {
        if (pass.spec.type == PassType::Pixel) { if (out.slot > maxRtvSlot) maxRtvSlot = out.slot; }
        else                                   { if (out.slot > maxUavSlot) maxUavSlot = out.slot; }
    }
    if (maxRtvSlot >= 0) rtvBindings.resize(maxRtvSlot + 1, nullptr);
    if (maxUavSlot >= 0) uavBindings.resize(maxUavSlot + 1, nullptr);
    Resource* primaryOut = nullptr;
    for (auto& out : pass.spec.outputs) {
        if (out.kind == OutputKind::GBufferRT) {
            // Direct bind to engine renderTargets[N].rtView. Used by composite
            // passes that need to write into an existing engine surface (e.g.
            // the HDR scene buffer) rather than an owned customResource.
            if (pass.spec.type != PassType::Pixel) continue;
            if (out.gbufferIndex < 0 || out.gbufferIndex >= 101) continue;
            auto* rt = g_rendererData->renderTargets[out.gbufferIndex].rtView;
            if (out.slot >= 0 && out.slot < (int)rtvBindings.size()) rtvBindings[out.slot] = rt;
            continue;
        }
        Resource* r = FindResource(out.resourceName);
        if (!r) continue;
        if (!primaryOut) primaryOut = r;
        if (pass.spec.type == PassType::Pixel) {
            if (out.slot >= 0 && out.slot < (int)rtvBindings.size()) rtvBindings[out.slot] = r->rtv;
        } else {
            if (out.slot >= 0 && out.slot < (int)uavBindings.size()) uavBindings[out.slot] = r->uav;
        }
    }

    // Determine output extent.
    //
    // Drive this from the actual OUTPUT TEXTURE, never from the saved
    // viewport — on the engine path (HookedBSBatchRendererDraw) the saved
    // viewport reflects the previous draw, which can be a downscaled bloom
    // blur target (e.g. ~1/8 of the screen). Using that would clip the
    // composite to a tiny rect even though the GBufferRT we write to is
    // full-screen.
    //
    //   1. First non-null output target -> its texture dimensions.
    //   2. If the user requested an explicit viewport scale (`viewport=screen/N`
    //      or `viewport=WxH`), apply that against the kMain backbuffer and
    //      override.
    //   3. Final fallback for safety.
    uint32_t outW = 0, outH = 0;
    for (auto& out : pass.spec.outputs) {
        REX::W32::ID3D11Texture2D* targetTex = nullptr;
        if (out.kind == OutputKind::GBufferRT) {
            if (out.gbufferIndex >= 0 && out.gbufferIndex < 101) {
                targetTex = g_rendererData->renderTargets[out.gbufferIndex].texture;
            }
        } else {
            if (auto* r = FindResource(out.resourceName)) targetTex = r->texture;
        }
        if (targetTex) {
            REX::W32::D3D11_TEXTURE2D_DESC d{};
            targetTex->GetDesc(&d);
            outW = d.width;
            outH = d.height;
            break;
        }
    }
    // Override if explicit viewport scale set (resolves against kMain's size).
    if (pass.spec.viewportMode != ScaleMode::Screen || pass.spec.viewportDiv > 1) {
        REX::W32::D3D11_TEXTURE2D_DESC bd{};
        if (g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture)
            g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture->GetDesc(&bd);
        uint32_t bw = bd.width ? bd.width : outW;
        uint32_t bh = bd.height ? bd.height : outH;
        ResolveScale(pass.spec.viewportMode, pass.spec.viewportDiv, 0, 0, bw, bh, outW, outH);
    }
    if (outW == 0) outW = saved.viewports[0].width  > 0 ? (uint32_t)saved.viewports[0].width  : 1;
    if (outH == 0) outH = saved.viewports[0].height > 0 ? (uint32_t)saved.viewports[0].height : 1;

    // ----- Pixel shader pass --------------------------------------------------
    if (pass.spec.type == PassType::Pixel) {
        auto* fsVS = GetFullscreenTriangleVS(device);
        // A PS pass without a bound RTV would do nothing useful (and could
        // unbind the engine's bindings as a side-effect of OMSetRenderTargets
        // with NumViews=0), so skip cleanly.
        bool anyRTV = false;
        for (auto* rt : rtvBindings) if (rt) { anyRTV = true; break; }
        // Caller (FirePass or FireBatch) owns the Restore — just bail.
        if (!fsVS || !pass.psShader || !anyRTV) return false;

        if (pass.spec.clearOnFire) {
            for (auto* rt : rtvBindings) if (rt) {
                float c[4] = { 0, 0, 0, 0 };
                context->ClearRenderTargetView(rt, c);
            }
        }

        context->OMSetRenderTargets((UINT)rtvBindings.size(), rtvBindings.data(), nullptr);
        REX::W32::D3D11_VIEWPORT vp{};
        vp.width = (float)outW; vp.height = (float)outH; vp.maxDepth = 1.0f;
        context->RSSetViewports(1, &vp);
        context->RSSetState(g_stateCache.GetRaster(device));
        context->OMSetBlendState(g_stateCache.GetBlend(device, pass.spec.blend), nullptr, 0xffffffff);
        context->OMSetDepthStencilState(g_stateCache.GetNoDepth(device), 0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetIndexBuffer(nullptr, REX::W32::DXGI_FORMAT_UNKNOWN, 0);
        context->VSSetShader(fsVS, nullptr, 0);
        context->CSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(pass.psShader, nullptr, 0);

        if (!srvBindings.empty()) context->PSSetShaderResources(0, (UINT)srvBindings.size(), srvBindings.data());
        // Re-publish the standard injected SRVs so the pass shader can read GFXInjected.
        if (g_customSRV) context->PSSetShaderResources(CUSTOMBUFFER_SLOT, 1, &g_customSRV);
        if (g_modularFloatsSRV) context->PSSetShaderResources(MODULAR_FLOATS_SLOT, 1, &g_modularFloatsSRV);
        if (g_modularIntsSRV)   context->PSSetShaderResources(MODULAR_INTS_SLOT, 1, &g_modularIntsSRV);
        if (g_modularBoolsSRV)  context->PSSetShaderResources(MODULAR_BOOLS_SLOT, 1, &g_modularBoolsSRV);
        LocalLightBridge::BindCustomPassResource(context, /*pixelStage=*/true);
        REX::W32::ID3D11SamplerState* samplers[2] = { g_passSamplerLinear, g_passSamplerPoint };
        context->PSSetSamplers(0, 2, samplers);
        context->Draw(3, 0);

        // Unbind RTVs to allow restore.
        REX::W32::ID3D11RenderTargetView* nullRTV[8] = {};
        context->OMSetRenderTargets(8, nullRTV, nullptr);
    }
    // ----- Compute pass --------------------------------------------------------
    else {
        // Caller owns the Restore — just bail.
        if (!pass.csShader) return false;
        context->CSSetShader(pass.csShader, nullptr, 0);
        if (!srvBindings.empty()) context->CSSetShaderResources(0, (UINT)srvBindings.size(), srvBindings.data());
        // Re-publish the standard injected SRVs so the CS pass can read
        // GFXInjected + Values.ini-backed modular shader values (vu_* /
        // ps_* knobs). The PS path does this just above; the CS path
        // previously only bound GFXInjected, so any CS pass referencing
        // a Values.ini knob would compile but sample garbage.
        if (g_customSRV)        context->CSSetShaderResources(CUSTOMBUFFER_SLOT, 1, &g_customSRV);
        if (g_modularFloatsSRV) context->CSSetShaderResources(MODULAR_FLOATS_SLOT, 1, &g_modularFloatsSRV);
        if (g_modularIntsSRV)   context->CSSetShaderResources(MODULAR_INTS_SLOT,   1, &g_modularIntsSRV);
        if (g_modularBoolsSRV)  context->CSSetShaderResources(MODULAR_BOOLS_SLOT,  1, &g_modularBoolsSRV);
        LocalLightBridge::BindCustomPassResource(context, /*pixelStage=*/false);
        if (!uavBindings.empty()) {
            std::vector<UINT> initial(uavBindings.size(), 0);
            context->CSSetUnorderedAccessViews(0, (UINT)uavBindings.size(), uavBindings.data(), initial.data());
        }
        REX::W32::ID3D11SamplerState* samplers[2] = { g_passSamplerLinear, g_passSamplerPoint };
        context->CSSetSamplers(0, 2, samplers);

        // Resolve dispatch geometry.
        UINT groups[3] = { 1, 1, 1 };
        REX::W32::D3D11_TEXTURE2D_DESC bd{};
        if (g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture)
            g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture->GetDesc(&bd);
        uint32_t bw = bd.width ? bd.width : outW;
        uint32_t bh = bd.height ? bd.height : outH;
        for (int i = 0; i < 3; ++i) {
            const auto& tg = pass.spec.threadGroups[i];
            switch (tg.mode) {
                case ScaleMode::Absolute:  groups[i] = std::max<uint32_t>(1, tg.value); break;
                case ScaleMode::Screen:    groups[i] = (i == 0 ? bw : (i == 1 ? bh : 1)); break;
                case ScaleMode::ScreenDiv: groups[i] = std::max<uint32_t>(1, (i == 0 ? bw : (i == 1 ? bh : 1)) / std::max<uint32_t>(1, tg.value)); break;
            }
        }
        context->Dispatch(groups[0], groups[1], groups[2]);
        // Unbind UAVs to release for restore.
        if (!uavBindings.empty()) {
            std::vector<REX::W32::ID3D11UnorderedAccessView*> nulls(uavBindings.size(), nullptr);
            std::vector<UINT> initial(uavBindings.size(), 0);
            context->CSSetUnorderedAccessViews(0, (UINT)nulls.size(), nulls.data(), initial.data());
        }
    }

    const uint64_t fires = pass.totalFireCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (pass.spec.oncePerFrame) {
        pass.lastFiredFrame.store(currentFrame, std::memory_order_release);
    }
    if (pass.spec.log) {
        // Rate-limit: every fire for the first 5, then every 600th frame
        // (~10s at 60fps). Avoids dumping 180 lines/sec when log=true is on
        // for several passes at once.
        if (fires <= 5 || (currentFrame % 600) == 0) {
            REX::INFO("CustomPass: fired '{}' (frame {}, total {})",
                pass.spec.name, currentFrame, fires);
        }
    }
    // No saved.Restore here — caller (FirePass single-pass wrapper or
    // FireBatch) owns the snapshot lifecycle.
    return true;
}

const DrawPassBatch* Registry::ResolveDrawPassBatchForShader(
    REX::W32::ID3D11PixelShader* originalPS,
    std::uint64_t* generation)
{
    const auto gen = drawPassCacheGeneration.load(std::memory_order_acquire);
    if (generation) {
        *generation = gen;
    }
    if (!originalPS || !hasDrawTimePasses.load(std::memory_order_acquire)) {
        return nullptr;
    }

    {
        std::lock_guard lk(mutex);
        auto it = drawBatchCache.find(originalPS);
        if (it != drawBatchCache.end()) {
            return it->second && !it->second->passes.empty() ? it->second.get() : nullptr;
        }
    }

    ShaderDefinition* matchedDef = nullptr;
    {
        std::shared_lock dlk(g_ShaderDB.mutex);
        auto it = g_ShaderDB.entries.find(originalPS);
        if (it != g_ShaderDB.entries.end() &&
            it->second.matched.load(std::memory_order_acquire)) {
            matchedDef = it->second.matchedDefinition;
        }
    }

    auto batch = std::make_unique<DrawPassBatch>();
    batch->originalPS = originalPS;
    batch->matchedDefinition = matchedDef;

    std::lock_guard lk(mutex);
    auto cached = drawBatchCache.find(originalPS);
    if (cached != drawBatchCache.end()) {
        return cached->second && !cached->second->passes.empty() ? cached->second.get() : nullptr;
    }

    if (matchedDef) {
        auto range = drawDefIndex.equal_range(matchedDef);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second) {
                batch->passes.push_back(it->second);
            }
        }
        std::stable_sort(batch->passes.begin(), batch->passes.end(),
            [](Pass* a, Pass* b) { return a->spec.priority < b->spec.priority; });
    }

    auto* result = batch.get();
    drawBatchCache.emplace(originalPS, std::move(batch));
    return result->passes.empty() ? nullptr : result;
}

bool Registry::FireResolvedDrawBatch(REX::W32::ID3D11DeviceContext* context,
                                     const DrawPassBatch* batch,
                                     std::uint64_t generation,
                                     const char* source)
{
    if (!context || !batch || batch->passes.empty()) {
        return false;
    }
    if (generation != drawPassCacheGeneration.load(std::memory_order_acquire)) {
        return false;
    }

    // Match-attempt counter (across all matched-def passes). When any
    // Draw-time pass has log=true, we dump bindings for the first N=8
    // matches so we can compare contexts.
    static std::atomic<uint32_t> matchSamples{ 0 };
    constexpr uint32_t kMaxMatchSamples = 8;

    // Diagnostic: when a Draw-time pass has log=true, dump engine bindings
    // for the first kMaxMatchSamples match attempts irrespective of which
    // pass eventually fires. We do this BEFORE FirePass so the captured
    // bindings reflect the engine state, not state our pass already mutated.
    bool wantDiag = false;
    for (auto* p : batch->passes) if (p && p->spec.log) { wantDiag = true; break; }
    if (wantDiag) {
        const uint32_t n = matchSamples.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= kMaxMatchSamples) {
            REX::INFO("CustomPass::OnBeforeDraw match #{} from {}: PS={} (matched-def fire context)",
                      n, source ? source : "?", static_cast<void*>(batch->originalPS));
            REX::W32::ID3D11RenderTargetView* rtvs[8] = {};
            REX::W32::ID3D11DepthStencilView* dsv = nullptr;
            context->OMGetRenderTargets(8, rtvs, &dsv);
            for (int i = 0; i < 8; ++i) {
                if (!rtvs[i]) continue;
                REX::W32::ID3D11Resource* res = nullptr;
                rtvs[i]->GetResource(&res);
                REX::INFO("  OM RTV{}: {}", i, IdentifyRenderTarget(res));
                if (res) res->Release();
                rtvs[i]->Release();
            }
            if (dsv) { dsv->Release(); }
            REX::W32::ID3D11ShaderResourceView* srvs[16] = {};
            context->PSGetShaderResources(0, 16, srvs);
            for (int i = 0; i < 16; ++i) {
                if (!srvs[i]) continue;
                REX::W32::ID3D11Resource* res = nullptr;
                srvs[i]->GetResource(&res);
                REX::INFO("  PS SRV t{}: {}", i, IdentifyRenderTarget(res));
                if (res) res->Release();
                srvs[i]->Release();
            }
        }
    }

    return FireSortedBatch(context, batch->passes);
}

bool Registry::OnBeforeDraw(REX::W32::ID3D11DeviceContext* context, const char* source) {
    if (!SHADERENGINE_EFFECTS_ON) return false;
    auto* originalPS = ::g_currentOriginalPixelShader.load(std::memory_order_acquire);
    std::uint64_t generation = 0;
    const auto* batch = ResolveDrawPassBatchForShader(originalPS, &generation);
    return FireResolvedDrawBatch(context, batch, generation, source);
}

bool Registry::OnBeforeShaderBound(REX::W32::ID3D11DeviceContext* context,
                                   REX::W32::ID3D11PixelShader* originalPS) {
    if (!SHADERENGINE_EFFECTS_ON) return false;
    if (!originalPS || !context) return false;
    std::vector<Pass*> matches;
    {
        std::shared_lock dlk(g_ShaderDB.mutex);
        auto it = g_ShaderDB.entries.find(originalPS);
        if (it == g_ShaderDB.entries.end()) return false;
        const std::string& uid = it->second.shaderUID;
        ShaderDefinition* matchedDef = it->second.matchedDefinition;
        std::lock_guard lk(mutex);
        // Prefer matched-definition triggers (collision-proof) — they fire
        // only when the engine binds a shader the matcher uniquely identified
        // as a particular [shaderId]. Fall back to raw UID triggers for users
        // who set triggerShaderUID directly.
        if (matchedDef) {
            auto defRange = defIndex.equal_range(matchedDef);
            for (auto p = defRange.first; p != defRange.second; ++p) matches.push_back(p->second);
        }
        auto range = uidIndex.equal_range(uid);
        for (auto p = range.first; p != range.second; ++p) matches.push_back(p->second);
    }
    if (matches.empty()) return false;
    return FireBatch(context, matches);
}

void Registry::OnFramePresent(REX::W32::ID3D11DeviceContext* context) {
    if (!SHADERENGINE_EFFECTS_ON) return;
    if (!context || !g_rendererData) return;
    ++currentFrame;

    // Allocate / reallocate resources for current backbuffer size.
    REX::W32::D3D11_TEXTURE2D_DESC bd{};
    if (g_rendererData->renderTargets[3].texture) g_rendererData->renderTargets[3].texture->GetDesc(&bd);
    if (bd.width == 0 || bd.height == 0) return;
    {
        std::lock_guard lk(mutex);
        for (auto& res : resources) res->EnsureAllocated(g_rendererData->device, bd.width, bd.height);
        for (auto& res : resources) {
            if (res->pingpongPartner) continue;
            if (res->spec.pingpongWith.empty()) continue;
            if (auto* p = FindResource(res->spec.pingpongWith)) {
                res->pingpongPartner = p;
                p->pingpongPartner = res.get();
            }
        }

        // copyAt=present
        for (auto& res : resources) {
            if (res->spec.copyAt != "present") continue;
            if (res->spec.copyFrom.rfind("renderTargets[", 0) == 0) {
                int idx = -1;
                try { idx = std::stoi(res->spec.copyFrom.substr(strlen("renderTargets["))); } catch (...) {}
                if (idx >= 0 && idx < 101) {
                    auto* src = g_rendererData->renderTargets[idx].texture;
                    if (src && res->texture) context->CopyResource(res->texture, src);
                }
            }
        }

        // clearOnPresent
        for (auto& res : resources) {
            if (!res->spec.clearOnPresent || !res->rtv) continue;
            context->ClearRenderTargetView(res->rtv, res->spec.clearColor);
        }

        // AtPresent passes
        for (auto& p : passes) {
            if (p->spec.trigger != TriggerKind::AtPresent) continue;
            FirePass(context, *p);
        }

        ApplyPingpong();
    }
}

void Registry::ApplyPingpong() {
    // Process each unordered pair only once.
    std::unordered_set<Resource*> done;
    for (auto& res : resources) {
        if (!res->pingpongPartner) continue;
        if (done.count(res.get()) || done.count(res->pingpongPartner)) continue;
        res->SwapContents(*res->pingpongPartner);
        done.insert(res.get());
        done.insert(res->pingpongPartner);
    }
}

void Registry::BindGlobalResourceSRVs(REX::W32::ID3D11DeviceContext* context, bool pixelStage) {
    if (!context) return;
    std::lock_guard lk(mutex);
    for (auto& res : resources) {
        if (!res->spec.globalBind || res->spec.srvSlot < 0 || !res->srv) continue;
        if (pixelStage) context->PSSetShaderResources((UINT)res->spec.srvSlot, 1, &res->srv);
        else            context->VSSetShaderResources((UINT)res->spec.srvSlot, 1, &res->srv);
    }
}

bool Registry::HasGlobalResourceBindings() const noexcept {
    return hasGlobalResourceBindings.load(std::memory_order_acquire);
}

REX::W32::ID3D11ShaderResourceView* Registry::GetResourceSRV(const std::string& name) {
    if (!g_rendererData || !g_rendererData->device) {
        return nullptr;
    }

    std::lock_guard lk(mutex);
    Resource* res = FindResource(name);
    if (!res) {
        return nullptr;
    }
    if (!res->srv) {
        REX::W32::D3D11_TEXTURE2D_DESC bd{};
        if (g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture) {
            g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].texture->GetDesc(&bd);
        }
        if (bd.width == 0 || bd.height == 0) {
            return nullptr;
        }
        res->EnsureAllocated(g_rendererData->device, bd.width, bd.height);
    }
    return res->srv;
}

void Registry::EnqueuePrecompileJobs() {
    if (!g_precompileWorker) return;
    // Snapshot pass pointers under the lock, release the lock, then enqueue.
    // The worker is stopped before any pass deletion (see
    // ReloadAllShaderDefinitions_Internal) so the captured pointers stay
    // valid for the duration of the queue.
    std::vector<Pass*> snapshot;
    {
        std::lock_guard lk(mutex);
        snapshot.reserve(passes.size());
        for (auto& p : passes) {
            if (p && p->spec.active && !p->spec.shaderFile.empty()) {
                snapshot.push_back(p.get());
            }
        }
    }
    auto* self = this;
    for (Pass* p : snapshot) {
        g_precompileWorker->Enqueue("customPass:" + p->spec.name,
                                     [self, p]{ self->EnsureCompiled(*p); });
    }
}

void Registry::StartFileWatchers() {
    std::lock_guard lk(mutex);
    for (auto& p : passes) {
        if (p->spec.shaderFile.empty()) continue;
        if (p->hlslWatcher) continue;
        Pass* raw = p.get();
        // Watcher just flips a dirty flag; the actual D3D11 object cleanup
        // and recompile happens on the main render thread inside FirePass.
        // This avoids cross-thread Release on shader objects (the immediate
        // context thread is the only safe owner) and avoids deadlocks with
        // the registry mutex.
        p->hlslWatcher = std::make_unique<FileWatcher>(p->spec.shaderFile, [raw]() {
            raw->reloadRequested.store(true, std::memory_order_release);
            REX::INFO("CustomPass[{}]: HLSL changed on disk, marked for reload", raw->spec.name);
        });
        p->hlslWatcher->Start();
    }
}

size_t Registry::ResourceCount() const { std::lock_guard lk(mutex); return resources.size(); }
size_t Registry::PassCount()    const { std::lock_guard lk(mutex); return passes.size(); }

}  // namespace CustomPass
