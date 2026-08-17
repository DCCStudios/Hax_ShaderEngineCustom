#include <Global.h>
#include <PCH.h>
#include <CustomPass.h>
#include <chrono>

#include "ContactShadowBridge.h"
#include <LocalLightBridge.h>
#include <SunCascadeBridge.h>
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
        { "R32_UINT",           REX::W32::DXGI_FORMAT_R32_UINT },
        { "R8_UNORM",           REX::W32::DXGI_FORMAT_R8_UNORM },
    };
    auto it = table.find(s);
    return (it != table.end()) ? it->second : REX::W32::DXGI_FORMAT_R11G11B10_FLOAT;
}

void ParseScale(const std::string& s, ScaleMode& mode, uint32_t& div, uint32_t& w, uint32_t& h) {
    mode = ScaleMode::Screen; div = 1; w = 0; h = 0;
    if (s.empty()) return;
    if (s == "saved")  { mode = ScaleMode::Saved; div = 1; return; }
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
        // Saved is resolved at fire time from the state snapshot (a live
        // viewport does not exist here); fall back to the backbuffer size so
        // a resource misconfigured with scale=saved still allocates sanely.
        case ScaleMode::Saved:     outW = backW; outH = backH; break;
    }
}

bool ParseInputBinding(const std::string& token, InputBinding& out) {
    // Format: "<slot>:<source>" where source is depth | currentRTV |
    // currentPSRV:N | customResource:NAME | gbufferRT:N | depthStencil:N
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
    // Depth-stencil targets are a separate array from renderTargets[]. The
    // shadow map array lives here (RT::Depth::kShadowMap = 6), which is what
    // world-space occlusion probes sample. Bound as srViewDepth, so the
    // consuming shader declares a Texture2DArray<float> and samples it with a
    // SamplerComparisonState.
    if (lowerSource.rfind("depthstencil:", 0) == 0) {
        out.kind = InputKind::DepthStencil;
        try { out.depthStencilIndex = std::stoi(source.substr(strlen("depthStencil:"))); } catch (...) { return false; }
        return true;
    }
    // File-backed texture: "N:file:relative/or/absolute.png". Relative paths
    // resolve against the pass's shader folder in the `input=` parse branch
    // (folderPath is not visible from here).
    if (lowerSource.rfind("file:", 0) == 0) {
        std::string path = source.substr(strlen("file:"));
        if (path.empty()) return false;
        out.kind = InputKind::File;
        out.fileTexture.file = path;
        out.fileTexture.slot = out.slot;
        return true;
    }
    return false;
}

bool ParseOutputBinding(const std::string& token, OutputBinding& out) {
    auto colon = token.find(':');
    if (colon == std::string::npos) return false;
    try { out.slot = std::stoi(token.substr(0, colon)); } catch (...) { return false; }
    std::string source = token.substr(colon + 1);
    const std::string lowerSource = ToLower(source);
    if (lowerSource.rfind("customresource:", 0) == 0) {
        out.kind = OutputKind::Resource;
        out.resourceName = source.substr(strlen("customResource:"));
        const std::string lowerName = ToLower(out.resourceName);
        const auto mipMarker = lowerName.rfind("@mip");
        if (mipMarker != std::string::npos) {
            const std::string mipText = out.resourceName.substr(mipMarker + 4);
            if (mipText.empty()) return false;
            try {
                size_t consumed = 0;
                const auto mip = std::stoul(mipText, &consumed);
                if (consumed != mipText.size()) return false;
                out.mipLevel = static_cast<uint32_t>(mip);
            } catch (...) {
                return false;
            }
            out.resourceName.resize(mipMarker);
            if (out.resourceName.empty()) return false;
        }
        return true;
    }
    if (lowerSource.rfind("gbufferrt:", 0) == 0) {
        out.kind = OutputKind::GBufferRT;
        try { out.gbufferIndex = std::stoi(source.substr(strlen("gbufferRT:"))); } catch (...) { return false; }
        return true;
    }
    if (lowerSource == "currentrtv") {
        out.kind = OutputKind::CurrentRTV;
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
    if (s.rfind("screenceil/", 0) == 0) {
        d.mode = ScaleMode::ScreenDiv;
        d.roundUp = true;
        try { d.value = static_cast<uint32_t>(std::stoul(s.substr(11))); } catch (...) { d.value = 1; }
    } else if (s.rfind("screen/", 0) == 0) {
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
    const bool isVolume = spec.depth > 1;
    auto targetFormat = spec.format;
    if (isVolume && spec.needUav && !SupportsVoxelTypedUavLoads()) {
        if (targetFormat == REX::W32::DXGI_FORMAT_R16_FLOAT) {
            targetFormat = REX::W32::DXGI_FORMAT_R32_FLOAT;
        } else if (targetFormat == REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT) {
            targetFormat = REX::W32::DXGI_FORMAT_R32_UINT;
        }
    }
    const bool hasTexture = isVolume ? texture3D != nullptr : texture != nullptr;
    if (hasTexture && targetW == width && targetH == height &&
        spec.depth == depth && targetFormat == allocatedFormat) return true;

    Release();
    width = targetW; height = targetH; depth = spec.depth;
    allocatedFormat = targetFormat;

    std::uint32_t bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
    if (spec.needRtv && !isVolume) bindFlags |= REX::W32::D3D11_BIND_RENDER_TARGET;
    if (spec.needUav) bindFlags |= REX::W32::D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = 0;
    if (isVolume) {
        REX::W32::D3D11_TEXTURE3D_DESC desc{};
        desc.width = width;
        desc.height = height;
        desc.depth = depth;
        desc.mipLevels = spec.mipLevels;
        desc.format = allocatedFormat;
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = bindFlags;
        hr = device->CreateTexture3D(&desc, nullptr, &texture3D);
        if (!REX::W32::SUCCESS(hr) || !texture3D) {
            REX::WARN("CustomPass::Resource[{}]: CreateTexture3D failed 0x{:08X}", spec.name, hr);
            Release(); return false;
        }
    } else {
        REX::W32::D3D11_TEXTURE2D_DESC desc{};
        desc.width = width;
        desc.height = height;
        desc.mipLevels = spec.mipLevels;
        desc.arraySize = 1;
        desc.format = allocatedFormat;
        desc.sampleDesc.count = 1;
        desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
        desc.bindFlags = bindFlags;
        hr = device->CreateTexture2D(&desc, nullptr, &texture);
        if (!REX::W32::SUCCESS(hr) || !texture) {
            REX::WARN("CustomPass::Resource[{}]: CreateTexture2D failed 0x{:08X}", spec.name, hr);
            Release(); return false;
        }
    }
    {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.format = allocatedFormat;
        if (isVolume) {
            sd.viewDimension = REX::W32::D3D11_SRV_DIMENSION_TEXTURE3D;
            sd.texture3D.mostDetailedMip = 0;
            sd.texture3D.mipLevels = spec.mipLevels;
        } else {
            sd.viewDimension = REX::W32::D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.texture2D.mostDetailedMip = 0;
            sd.texture2D.mipLevels = spec.mipLevels;
        }
        auto* resource = isVolume
            ? static_cast<REX::W32::ID3D11Resource*>(texture3D)
            : static_cast<REX::W32::ID3D11Resource*>(texture);
        hr = device->CreateShaderResourceView(resource, &sd, &srv);
        if (!REX::W32::SUCCESS(hr)) {
            REX::WARN("CustomPass::Resource[{}]: CreateShaderResourceView failed 0x{:08X}", spec.name, hr);
            Release(); return false;
        }
    }
    if (spec.needRtv && !isVolume) {
        REX::W32::D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.format        = allocatedFormat;
        rd.viewDimension = REX::W32::D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = device->CreateRenderTargetView(texture, &rd, &rtv);
        if (!REX::W32::SUCCESS(hr)) {
            REX::WARN("CustomPass::Resource[{}]: CreateRenderTargetView failed 0x{:08X}", spec.name, hr);
            Release(); return false;
        }
    }
    if (spec.needUav) {
        mipUavs.reserve(spec.mipLevels);
        for (uint32_t mip = 0; mip < spec.mipLevels; ++mip) {
            REX::W32::D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.format = allocatedFormat;
            if (isVolume) {
                ud.viewDimension = REX::W32::D3D11_UAV_DIMENSION_TEXTURE3D;
                ud.texture3D.mipSlice = mip;
                ud.texture3D.firstWSlice = 0;
                ud.texture3D.wSize = std::max<uint32_t>(1, depth >> mip);
            } else {
                ud.viewDimension = REX::W32::D3D11_UAV_DIMENSION_TEXTURE2D;
                ud.texture2D.mipSlice = mip;
            }
            REX::W32::ID3D11UnorderedAccessView* mipUav = nullptr;
            auto* resource = isVolume
                ? static_cast<REX::W32::ID3D11Resource*>(texture3D)
                : static_cast<REX::W32::ID3D11Resource*>(texture);
            hr = device->CreateUnorderedAccessView(resource, &ud, &mipUav);
            if (!REX::W32::SUCCESS(hr) || !mipUav) {
                REX::WARN("CustomPass::Resource[{}]: CreateUnorderedAccessView mip {} failed 0x{:08X}", spec.name, mip, hr);
                Release(); return false;
            }
            mipUavs.push_back(mipUav);
        }
        uav = mipUavs.front();
    }
    REX::INFO(
        "CustomPass::Resource[{}]: allocated {}x{}x{} mips={} format={} (domain={})",
        spec.name,
        width,
        height,
        depth,
        spec.mipLevels,
        static_cast<uint32_t>(allocatedFormat),
        spec.renderDomain ? "render" : "allocation");
    return true;
}

void Resource::Release() {
    for (auto* view : mipUavs) if (view) view->Release();
    mipUavs.clear();
    uav = nullptr;
    if (rtv) { rtv->Release(); rtv = nullptr; }
    if (srv) { srv->Release(); srv = nullptr; }
    if (texture) { texture->Release(); texture = nullptr; }
    if (texture3D) { texture3D->Release(); texture3D = nullptr; }
    width = height = depth = 0;
    allocatedFormat = REX::W32::DXGI_FORMAT_UNKNOWN;
}

void Resource::SwapContents(Resource& other) {
    std::swap(texture, other.texture);
    std::swap(texture3D, other.texture3D);
    std::swap(rtv, other.rtv);
    std::swap(srv, other.srv);
    std::swap(uav, other.uav);
    std::swap(mipUavs, other.mipUavs);
    std::swap(width, other.width);
    std::swap(height, other.height);
    std::swap(depth, other.depth);
    std::swap(allocatedFormat, other.allocatedFormat);
}

// --- Pass ---------------------------------------------------------------

void Pass::Release() {
    if (hlslWatcher) { hlslWatcher->Stop(); hlslWatcher.reset(); }
    for (auto& in : spec.inputs) {
        if (in.kind == InputKind::File) in.fileTexture.Release();
    }
    if (psShader) { psShader->Release(); psShader = nullptr; }
    if (csShader) { csShader->Release(); csShader = nullptr; }
    if (compiledBlob) { compiledBlob->Release(); compiledBlob = nullptr; }
    for (auto& timing : gpuTiming) {
        if (timing.disjoint) { timing.disjoint->Release(); timing.disjoint = nullptr; }
        if (timing.begin) { timing.begin->Release(); timing.begin = nullptr; }
        if (timing.end) { timing.end->Release(); timing.end = nullptr; }
        timing.submittedFrame = 0;
        timing.pending = false;
    }
    gpuLastMs.store(0.0f, std::memory_order_relaxed);
    gpuAverageMs.store(0.0f, std::memory_order_relaxed);
    gpuTimingSamples.store(0, std::memory_order_relaxed);
    gpuTimingUnavailable = false;
    gpuTimingFailureLogged = false;
    compileTried = false; compileFailed = false;
}

namespace {
bool EnsureGpuTimingQueries(Pass& pass, REX::W32::ID3D11Device* device)
{
    if (!pass.spec.profileGpu || pass.gpuTimingUnavailable || !device) {
        return false;
    }
    if (pass.gpuTiming.front().disjoint) {
        return true;
    }

    const REX::W32::D3D11_QUERY_DESC disjointDesc{
        REX::W32::D3D11_QUERY_TIMESTAMP_DISJOINT,
        0
    };
    const REX::W32::D3D11_QUERY_DESC timestampDesc{
        REX::W32::D3D11_QUERY_TIMESTAMP,
        0
    };
    bool ready = true;
    for (auto& timing : pass.gpuTiming) {
        ready = ready && REX::W32::SUCCESS(
            device->CreateQuery(&disjointDesc, &timing.disjoint));
        ready = ready && REX::W32::SUCCESS(
            device->CreateQuery(&timestampDesc, &timing.begin));
        ready = ready && REX::W32::SUCCESS(
            device->CreateQuery(&timestampDesc, &timing.end));
        if (!ready) {
            break;
        }
    }
    if (ready) {
        return true;
    }

    for (auto& timing : pass.gpuTiming) {
        if (timing.disjoint) { timing.disjoint->Release(); timing.disjoint = nullptr; }
        if (timing.begin) { timing.begin->Release(); timing.begin = nullptr; }
        if (timing.end) { timing.end->Release(); timing.end = nullptr; }
        timing.pending = false;
    }
    pass.gpuTimingUnavailable = true;
    if (!pass.gpuTimingFailureLogged) {
        REX::WARN(
            "CustomPass[{}]: asynchronous GPU timestamp queries unavailable; profiling disabled",
            pass.spec.name);
        pass.gpuTimingFailureLogged = true;
    }
    return false;
}

Pass::GpuTimingSlot* BeginGpuTiming(
    Pass& pass,
    REX::W32::ID3D11Device* device,
    REX::W32::ID3D11DeviceContext* context,
    uint32_t frame)
{
    if (!context || !EnsureGpuTimingQueries(pass, device)) {
        return nullptr;
    }

    auto& timing = pass.gpuTiming[frame % pass.gpuTiming.size()];
    if (timing.pending) {
        // Never overwrite an unresolved query. Skipping a diagnostic sample
        // is preferable to forcing the driver to flush or wait.
        return nullptr;
    }
    context->Begin(timing.disjoint);
    context->End(timing.begin);
    timing.submittedFrame = frame;
    return &timing;
}

void EndGpuTiming(
    REX::W32::ID3D11DeviceContext* context,
    Pass::GpuTimingSlot* timing)
{
    if (!context || !timing) {
        return;
    }
    context->End(timing->end);
    context->End(timing->disjoint);
    timing->pending = true;
}

void PollGpuTiming(
    Pass& pass,
    REX::W32::ID3D11DeviceContext* context,
    uint32_t frame)
{
    if (!pass.spec.profileGpu || !context || pass.gpuTimingUnavailable) {
        return;
    }

    constexpr std::uint32_t kNoFlush =
        REX::W32::D3D11_ASYNC_GETDATA_DONOTFLUSH;
    for (auto& timing : pass.gpuTiming) {
        if (!timing.pending || frame - timing.submittedFrame < 2) {
            continue;
        }

        REX::W32::D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        const HRESULT disjointResult = context->GetData(
            timing.disjoint,
            &disjoint,
            sizeof(disjoint),
            kNoFlush);
        if (disjointResult == S_FALSE) {
            continue;
        }
        if (FAILED(disjointResult)) {
            timing.pending = false;
            continue;
        }

        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        const HRESULT beginResult = context->GetData(
            timing.begin, &begin, sizeof(begin), kNoFlush);
        const HRESULT endResult = context->GetData(
            timing.end, &end, sizeof(end), kNoFlush);
        if (beginResult == S_FALSE || endResult == S_FALSE) {
            continue;
        }
        timing.pending = false;
        if (FAILED(beginResult) || FAILED(endResult) || disjoint.disjoint ||
            disjoint.frequency == 0 || end < begin) {
            continue;
        }

        const float milliseconds = static_cast<float>(
            static_cast<double>(end - begin) * 1000.0 /
            static_cast<double>(disjoint.frequency));
        const std::uint64_t samples =
            pass.gpuTimingSamples.fetch_add(1, std::memory_order_relaxed) + 1;
        const float previous =
            pass.gpuAverageMs.load(std::memory_order_relaxed);
        const float average = samples == 1 ? milliseconds :
            previous + 0.15f * (milliseconds - previous);
        pass.gpuLastMs.store(milliseconds, std::memory_order_relaxed);
        pass.gpuAverageMs.store(average, std::memory_order_relaxed);
    }
}
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
        else if (lk == "depth")           { try { res->spec.depth = std::max<uint32_t>(1, static_cast<uint32_t>(std::stoul(value))); } catch (...) {} }
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
            } else if (value == "afterDeferredLights") {
                pass->spec.trigger = TriggerKind::AfterDeferredLights;
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
            for (auto& tok : parts) {
                InputBinding b;
                if (!ParseInputBinding(tok, b)) continue;
                if (b.kind == InputKind::File && b.fileTexture.file.is_relative()) {
                    b.fileTexture.file = folderPath / b.fileTexture.file;
                }
                pass->spec.inputs.push_back(b);
            }
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
        else if (lk == "profilegpu" || lk == "profile")
            pass->spec.profileGpu = (ToLower(value) == "true" || value == "1");
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
        .shaderFolder    = pass.spec.shaderFile.parent_path(),
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

// Log the engine's bound state at trigger time. Fires on the pass's first
// fire, and again whenever the engine's RTV0 RESOURCE changes (rate-limited
// to 1 per 2s) - the engine composites the scene into different textures on
// different frames, and the binding set that accompanies the alternate
// target is exactly the information a first-fire-only dump misses.
void LogEngineBindings(const Pass& pass, REX::W32::ID3D11DeviceContext* ctx, uint64_t fireCount) {
    if (!ctx) return;

    REX::W32::ID3D11RenderTargetView* rtvs[8] = {};
    REX::W32::ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(8, rtvs, &dsv);

    // Pointer identity only, never dereferenced later.
    static const void* s_lastRtv0Resource = nullptr;
    static std::uint64_t s_lastForcedDumpMs = 0;
    const void* rtv0Resource = nullptr;
    if (rtvs[0]) {
        REX::W32::ID3D11Resource* res = nullptr;
        rtvs[0]->GetResource(&res);
        rtv0Resource = res;
        if (res) res->Release();
    }
    bool due = fireCount == 1;
    if (!due && rtv0Resource != s_lastRtv0Resource) {
        const auto nowMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (nowMs - s_lastForcedDumpMs > 2000) {
            s_lastForcedDumpMs = nowMs;
            due = true;
        }
    }
    s_lastRtv0Resource = rtv0Resource;
    if (!due) {
        for (int i = 0; i < 8; ++i) if (rtvs[i]) rtvs[i]->Release();
        if (dsv) dsv->Release();
        return;
    }

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

    REX::W32::ID3D11ShaderResourceView* srvs[32] = {};
    ctx->PSGetShaderResources(0, 32, srvs);
    for (int i = 0; i < 32; ++i) {
        if (!srvs[i]) continue;
        REX::W32::ID3D11Resource* res = nullptr;
        srvs[i]->GetResource(&res);
        // Include dimensions: two same-purpose textures at different
        // resolutions (native scene vs upscaler proxy) both identify as
        // "(unknown)", and the size is what tells them apart.
        std::string name = IdentifyRenderTarget(res);
        if (res) {
            REX::W32::ID3D11Texture2D* tex = nullptr;
            res->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                reinterpret_cast<void**>(&tex));
            if (tex) {
                REX::W32::D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                name += std::format(" {}x{}", d.width, d.height);
                tex->Release();
            }
            res->Release();
        }
        REX::INFO("  PS SRV t{}: {}", i, name);
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
REX::W32::ID3D11SamplerState* g_passSamplerLinearWrap = nullptr;
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
    if (!g_passSamplerLinearWrap) {
        REX::W32::D3D11_SAMPLER_DESC d{};
        d.filter = REX::W32::D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.addressU = d.addressV = d.addressW =
            REX::W32::D3D11_TEXTURE_ADDRESS_WRAP;
        d.maxLOD = (std::numeric_limits<float>::max)();
        dev->CreateSamplerState(&d, &g_passSamplerLinearWrap);
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

// Evaluate the optional `activeWhen` runtime gate. Commas and `&&` both form
// an AND-list, and each term may start with `!`. Resolved ShaderValue pointers
// are cached on first use so the render-thread fast path only reads bools.
static bool EvaluateActiveWhen(Pass& pass) {
    const std::string& spec = pass.spec.activeWhen;
    if (spec.empty()) return true;

    if (!pass.activeWhenChecked) {
        std::string normalized = spec;
        for (size_t pos = 0; (pos = normalized.find("&&", pos)) != std::string::npos;) {
            normalized.replace(pos, 2, ",");
        }
        std::stringstream terms(normalized);
        std::string token;
        while (std::getline(terms, token, ',')) {
            if (token.empty()) continue;
            const bool negate = token[0] == '!';
            const std::string id = negate ? token.substr(1) : token;
            if (id.empty()) continue;
            ShaderValue* resolved = nullptr;
            for (auto* sv : g_shaderSettings.GetBoolShaderValues()) {
                if (sv && sv->id == id) { resolved = sv; break; }
            }
            if (!resolved) {
                REX::WARN("CustomPass[{}]: activeWhen term '{}' did not resolve to a Values.ini bool; that term will fire-open",
                    pass.spec.name, token);
            }
            pass.activeWhenTerms.push_back({ resolved, negate });
        }
        pass.activeWhenChecked  = true;
    }

    for (const auto& term : pass.activeWhenTerms) {
        auto* sv = static_cast<ShaderValue*>(term.shaderValue);
        if (!sv) continue;
        const bool value = term.negate ? !sv->current.b : sv->current.b;
        if (!value) return false;
    }
    return true;
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
            case InputKind::Depth: {
                g_depthSRV = ShaderResources::GetDepthBufferSRV_Internal();
                s = g_depthSRV;
                // Transition log: which texture "depth" actually resolves to.
                // The scene can render into an upscaler proxy while the depth
                // TABLE entry keeps pointing at the native-size buffer, in
                // which case every consumer of this input samples depth the
                // current frame never wrote - the contact raymarch then sees
                // no occluders anywhere and its whole output silently
                // disappears. Pointer identity only, never dereferenced.
                {
                    static const void* s_lastDepthRes =
                        reinterpret_cast<const void*>(~uintptr_t{0});
                    const void* depthRes = nullptr;
                    uint32_t dw = 0, dh = 0;
                    if (s) {
                        REX::W32::ID3D11Resource* res = nullptr;
                        s->GetResource(&res);
                        if (res) {
                            depthRes = res;
                            REX::W32::ID3D11Texture2D* tex = nullptr;
                            res->QueryInterface(
                                REX::W32::IID_ID3D11Texture2D,
                                reinterpret_cast<void**>(&tex));
                            if (tex) {
                                REX::W32::D3D11_TEXTURE2D_DESC d{};
                                tex->GetDesc(&d);
                                dw = d.width;
                                dh = d.height;
                                tex->Release();
                            }
                            res->Release();
                        }
                    }
                    if (depthRes != s_lastDepthRes) {
                        s_lastDepthRes = depthRes;
                        REX::INFO(
                            "CustomPass[{}]: depth input resolved to {} ({}x{})",
                            pass.spec.name, depthRes, dw, dh);
                    }
                }
                break;
            }
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
            case InputKind::DepthStencil: {
                // Stays nullptr when the engine has not allocated the target
                // this frame (the shadow array is absent in some interiors and
                // during loading). Consuming shaders must treat a null bind as
                // "fully visible" rather than "fully occluded".
                if (in.depthStencilIndex >= 0 &&
                    in.depthStencilIndex < (int)RT::idx(RT::Depth::kCount)) {
                    s = g_rendererData->depthStencilTargets[in.depthStencilIndex].srViewDepth;
                }
                // One-shot report per index. A null SRV here is silent on the
                // GPU: samples return 0, every depth comparison resolves the
                // same way, and the consuming field looks uniformly wrong
                // rather than absent - which is indistinguishable from a
                // working field with nothing to show.
                {
                    static std::array<bool, 16> s_reported{};
                    const int idx = in.depthStencilIndex;
                    if (idx >= 0 && idx < 16 && !s_reported[idx]) {
                        s_reported[idx] = true;
                        if (!s) {
                            REX::WARN(
                                "CustomPass: depthStencil:{} has no srViewDepth; "
                                "shader samples will read 0",
                                idx);
                        } else {
                            REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC d{};
                            s->GetDesc(&d);
                            REX::INFO(
                                "CustomPass: depthStencil:{} bound, srv dimension={} "
                                "format={} arraySize={}",
                                idx,
                                static_cast<int>(d.viewDimension),
                                static_cast<int>(d.format),
                                d.texture2DArray.arraySize);
                        }
                    }
                }
                break;
            }
            case InputKind::File: {
                // Lazy-loaded on first fire, exactly like a replacement
                // shader's bindTexture. Load failure logs once (inside the
                // loader) and the slot stays null; the consuming shader must
                // treat a null bind (samples return 0) as "feature off".
                ShaderResources::EnsureFileTextureSRV(device, in.fileTexture);
                s = in.fileTexture.srv;
                break;
            }
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
        if (out.kind == OutputKind::CurrentRTV) {
            // Bind whatever the engine had on OM slot 0 when the trigger
            // fired. Capture AddRef'd saved.rtvs[0] and Restore releases it
            // after the batch, so the reference is alive for the whole fire.
            // The target's identity is unknown by design (at
            // afterDeferredLights it is not in renderTargets[] at all), so
            // guard on the one thing the pass requires: a real 2D texture of
            // plausible size. On any mismatch the slot stays null and the
            // anyRTV check below skips the pass instead of corrupting state.
            if (pass.spec.type != PassType::Pixel) continue;
            auto* rt = saved.rtvs[0];
            bool usable = false;
            REX::W32::ID3D11Resource* identity = nullptr;
            uint32_t descW = 0, descH = 0;
            int descFormat = -1;
            if (rt) {
                REX::W32::ID3D11Resource* res = nullptr;
                rt->GetResource(&res);
                if (res) {
                    identity = res;
                    REX::W32::ID3D11Texture2D* tex = nullptr;
                    res->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                        reinterpret_cast<void**>(&tex));
                    if (tex) {
                        REX::W32::D3D11_TEXTURE2D_DESC d{};
                        tex->GetDesc(&d);
                        usable = d.width >= 16 && d.height >= 16;
                        descW = d.width;
                        descH = d.height;
                        descFormat = static_cast<int>(d.format);
                        tex->Release();
                    }
                    res->Release();
                }
            }
            // Transition log: the target's identity is unknown by design, so
            // the one thing worth recording is when it CHANGES - a full-frame
            // effect dropout at a specific camera angle would show here as the
            // engine swapping (or unbinding) its composite destination.
            // `s_lastIdentity` stores the pointer VALUE for comparison only
            // and is never dereferenced; a destroyed-and-reallocated texture
            // at the same address logs nothing, which is acceptable for a
            // diagnostic. Static is fine: render-thread only, and shared
            // across the (currently one) pass using currentRTV.
            {
                static REX::W32::ID3D11Resource* s_lastIdentity =
                    reinterpret_cast<REX::W32::ID3D11Resource*>(~uintptr_t{0});
                if (identity != s_lastIdentity) {
                    s_lastIdentity = identity;
                    REX::INFO(
                        "CustomPass[{}]: currentRTV target changed: res={} "
                        "{}x{} fmt={} usable={}",
                        pass.spec.name, static_cast<void*>(identity),
                        descW, descH, descFormat, usable);
                }
            }
            if (usable && out.slot >= 0 && out.slot < (int)rtvBindings.size()) {
                rtvBindings[out.slot] = rt;
            }
            continue;
        }
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
            if (out.mipLevel >= r->mipUavs.size()) {
                REX::WARN("CustomPass[{}]: output '{}' requests mip {} but resource has {} UAV mip(s)",
                    pass.spec.name, r->spec.name, out.mipLevel, r->mipUavs.size());
                return false;
            }
            if (out.slot >= 0 && out.slot < (int)uavBindings.size()) {
                uavBindings[out.slot] = r->mipUavs[out.mipLevel];
            }
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
        if (out.kind == OutputKind::CurrentRTV) {
            // Size from the live target itself. Released immediately after
            // GetDesc below via the shared targetTex handling being skipped -
            // so query the desc here and continue the loop directly.
            if (saved.rtvs[0]) {
                REX::W32::ID3D11Resource* res = nullptr;
                saved.rtvs[0]->GetResource(&res);
                if (res) {
                    REX::W32::ID3D11Texture2D* tex = nullptr;
                    res->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                        reinterpret_cast<void**>(&tex));
                    if (tex) {
                        REX::W32::D3D11_TEXTURE2D_DESC d{};
                        tex->GetDesc(&d);
                        outW = d.width;
                        outH = d.height;
                        tex->Release();
                    }
                    res->Release();
                }
            }
            if (outW > 0) break;
            continue;
        }
        if (out.kind == OutputKind::GBufferRT) {
            if (out.gbufferIndex >= 0 && out.gbufferIndex < 101) {
                targetTex = g_rendererData->renderTargets[out.gbufferIndex].texture;
            }
        } else {
            if (auto* r = FindResource(out.resourceName)) {
                if (r->texture3D) {
                    outW = std::max<uint32_t>(1, r->width >> out.mipLevel);
                    outH = std::max<uint32_t>(1, r->height >> out.mipLevel);
                    break;
                }
                targetTex = r->texture;
            }
        }
        if (targetTex) {
            REX::W32::D3D11_TEXTURE2D_DESC d{};
            targetTex->GetDesc(&d);
            outW = d.width;
            outH = d.height;
            break;
        }
    }
    // viewport=saved: rasterize over the engine's own viewport as captured
    // when the trigger fired. At a beforeDrawForHook trigger that is the
    // hooked draw's viewport - during the imagespace HDR phase (tonemap and
    // earlier) the dynamic-resolution SUBRECT of the allocation, so the
    // pass's fullscreen UV becomes the engine's scene-logical UV and its
    // writes land only on texels the scene actually occupies. Falls back to
    // the output texture's size when the snapshot has no usable viewport.
    if (pass.spec.viewportMode == ScaleMode::Saved) {
        if (saved.viewports[0].width >= 1.0f && saved.viewports[0].height >= 1.0f) {
            outW = (uint32_t)saved.viewports[0].width;
            outH = (uint32_t)saved.viewports[0].height;
        }
        // Transition log: the saved viewport IS the diagnosis surface for
        // DRS-space bugs (full allocation here means the engine was not in a
        // subrect and the mapping is identity). Render-thread only.
        {
            static uint64_t s_lastDims = ~0ull;
            const uint64_t dims = (uint64_t(outW) << 32) | outH;
            if (dims != s_lastDims) {
                s_lastDims = dims;
                REX::INFO("CustomPass[{}]: saved viewport {}x{}",
                    pass.spec.name, outW, outH);
            }
        }
    }
    // Override if explicit viewport scale set (resolves against kMain's size).
    else if (pass.spec.viewportMode != ScaleMode::Screen || pass.spec.viewportDiv > 1) {
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
        SunCascadeBridge::BindCustomPassResource(context, /*pixelStage=*/true);
        ContactShadowBridge::BindCustomPassResource(context, /*pixelStage=*/true);
        REX::W32::ID3D11SamplerState* samplers[3] = {
            g_passSamplerLinear,
            g_passSamplerPoint,
            g_passSamplerLinearWrap
        };
        context->PSSetSamplers(0, 3, samplers);
        auto* gpuTiming = BeginGpuTiming(
            pass, device, context, currentFrame);
        context->Draw(3, 0);
        EndGpuTiming(context, gpuTiming);

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
        SunCascadeBridge::BindCustomPassResource(context, /*pixelStage=*/false);
        ContactShadowBridge::BindCustomPassResource(context, /*pixelStage=*/false);
        if (!uavBindings.empty()) {
            std::vector<UINT> initial(uavBindings.size(), 0);
            context->CSSetUnorderedAccessViews(0, (UINT)uavBindings.size(), uavBindings.data(), initial.data());
        }
        REX::W32::ID3D11SamplerState* samplers[3] = {
            g_passSamplerLinear,
            g_passSamplerPoint,
            g_passSamplerLinearWrap
        };
        context->CSSetSamplers(0, 3, samplers);

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
                case ScaleMode::ScreenDiv: {
                    const auto extent = i == 0 ? bw : (i == 1 ? bh : 1);
                    const auto divisor = std::max<uint32_t>(1, tg.value);
                    groups[i] = std::max<uint32_t>(1,
                        tg.roundUp ? (extent + divisor - 1) / divisor
                                   : extent / divisor);
                    break;
                }
            }
        }
        auto* gpuTiming = BeginGpuTiming(
            pass, device, context, currentFrame);
        context->Dispatch(groups[0], groups[1], groups[2]);
        EndGpuTiming(context, gpuTiming);
        // Unbind UAVs to release for restore.
        if (!uavBindings.empty()) {
            std::vector<REX::W32::ID3D11UnorderedAccessView*> nulls(uavBindings.size(), nullptr);
            std::vector<UINT> initial(uavBindings.size(), 0);
            context->CSSetUnorderedAccessViews(0, (UINT)nulls.size(), nulls.data(), initial.data());
        }
    }

    const uint64_t fires = pass.totalFireCount.fetch_add(1, std::memory_order_relaxed) + 1;
    // Unconditional: the oncePerFrame GATE above only reads this when that
    // flag is set, but the afterDeferredLights batch diagnostic reads it as
    // "did this pass fire this frame" - with the old conditional store, a
    // oncePerFrame=false pass logged as SKIPPED forever while firing fine,
    // which sent a live debugging session down the wrong path (2026-08-14).
    pass.lastFiredFrame.store(currentFrame, std::memory_order_release);
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
        for (auto& pass : passes) {
            PollGpuTiming(*pass, context, currentFrame);
        }
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
            if (!res->spec.clearOnPresent) continue;
            if (res->rtv) {
                context->ClearRenderTargetView(res->rtv, res->spec.clearColor);
            } else if (res->uav) {
                if (res->allocatedFormat == REX::W32::DXGI_FORMAT_R32_UINT) {
                    const std::uint32_t clear[4] = {
                        static_cast<std::uint32_t>(res->spec.clearColor[0]),
                        static_cast<std::uint32_t>(res->spec.clearColor[1]),
                        static_cast<std::uint32_t>(res->spec.clearColor[2]),
                        static_cast<std::uint32_t>(res->spec.clearColor[3])
                    };
                    context->ClearUnorderedAccessViewUint(res->uav, clear);
                } else {
                    context->ClearUnorderedAccessViewFloat(res->uav, res->spec.clearColor);
                }
            }
        }

        // AtPresent passes
        for (auto& p : passes) {
            if (p->spec.trigger != TriggerKind::AtPresent) continue;
            FirePass(context, *p);
        }

        ApplyPingpong();
    }
}

void Registry::FireAfterDeferredLights(REX::W32::ID3D11DeviceContext* context) {
    if (!context) return;
    std::vector<Pass*> matches;
    {
        std::lock_guard lk(mutex);
        for (auto& p : passes) {
            if (p->spec.trigger != TriggerKind::AfterDeferredLights) continue;
            if (!p->spec.active) continue;
            matches.push_back(p.get());
        }
    }
    const bool fired = matches.empty() ? false : FireBatch(context, matches);

    // Throttled. Separates the two ways this can look identical in game -
    // "no pass matched the trigger" from "passes fired but wrote somewhere
    // that gets discarded" - which is otherwise only distinguishable by
    // reading the GPU profiler, and cost a full test cycle to guess at.
    static std::atomic<std::uint64_t> lastLogMs{0};
    const auto nowMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto previous = lastLogMs.load(std::memory_order_relaxed);
    if (nowMs - previous > 5000u) {
        lastLogMs.store(nowMs, std::memory_order_relaxed);
        std::string names;
        for (auto* p : matches) {
            if (!names.empty()) names += ", ";
            names += p->spec.name;
            names += p->lastFiredFrame.load(std::memory_order_relaxed) ==
                     currentFrame ? "(fired)" : "(SKIPPED)";
        }
        REX::INFO(
            "CustomPass: afterDeferredLights - {} matched, FireBatch={} | {}",
            matches.size(), fired, names.empty() ? "<none>" : names);
    }
}

void FireAfterDeferredLightsPasses(REX::W32::ID3D11DeviceContext* context) {
    g_registry.FireAfterDeferredLights(context);
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

std::size_t Registry::RequestReloadAll() {
    std::lock_guard lk(mutex);
    std::size_t marked = 0;
    for (auto& p : passes) {
        if (!p || p->spec.shaderFile.empty()) continue;
        p->reloadRequested.store(true, std::memory_order_release);
        ++marked;
    }
    return marked;
}

size_t Registry::ResourceCount() const { std::lock_guard lk(mutex); return resources.size(); }
size_t Registry::PassCount()    const { std::lock_guard lk(mutex); return passes.size(); }

}  // namespace CustomPass
