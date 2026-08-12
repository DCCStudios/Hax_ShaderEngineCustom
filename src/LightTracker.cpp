#include <Global.h>
#include <PCH.h>
#include <RenderTargets.h>
#include "LightTracker.h"

// g_currentOriginalPixelShader is owned by Plugin.cpp; we want the
// pre-replacement PS pointer so the ShaderDB lookup hits the engine shader
// (matched defs, original UID) rather than our injected replacement.
extern std::atomic<REX::W32::ID3D11PixelShader*> g_currentOriginalPixelShader;

namespace LightTracker {

namespace {

enum class State { Idle, Armed, Capturing };

constexpr int  kHotkey            = VK_MULTIPLY;  // Numpad *
constexpr UINT kStagingByteWidth  = 512;          // covers cb2 (max 480 in F4 deferred-light passes)
constexpr UINT kSrvCount          = 8;
// cb2[0..20]. [0..8] is the light/SH bank; the cascaded sun variant also
// uses [10] = cascade end distances, [11..13]/[14..16]/[17..19] = the three
// cascade projections, [20].zw = shadow map texel scale. Verified against
// HachiToon/pixelDeferredLight0034A5F8.hlsl. Capturing through 20 lets a dump
// validate CPU-side cascade matrices against what the engine hands its own
// shader.
constexpr int  kCb2VecCount       = 21;

State                              g_state          = State::Idle;
bool                               g_prevHotkeyDown = false;
std::uint64_t                      g_frameCounter   = 0;
std::uint32_t                      g_drawIdx        = 0;
std::uint32_t                      g_emittedRows    = 0;
std::ofstream                      g_csv;
std::filesystem::path              g_csvPath;
REX::W32::ID3D11Buffer*            g_stagingCB      = nullptr;

// Pending-capture state. We snapshot once at this PS bind (cb2 is fresh for
// passes where the engine binds cb2 first then PSSetShader — local lights)
// and again at the *next* PS bind (cb2 is fresh for passes where the engine
// binds PSSetShader first then writes cb2 — directional/ambient). Both
// snapshots go into the same CSV row so the reader can pick whichever cb2
// looks like real per-pass data for the given UID.
struct PendingCapture {
    bool          valid          = false;
    std::string   uid;
    std::string   definitionId;
    bool          hasReplacement = false;
    std::uint64_t frame          = 0;
    std::uint32_t bindIdx        = 0;
    // At-bind snapshot — captured immediately when MyPSSetShader fired.
    std::string                              rtvName  = "(none)";
    REX::W32::D3D11_RENDER_TARGET_BLEND_DESC rt0Blend{};
    REX::W32::D3D11_RECT                     scissor{};
    REX::W32::D3D11_VIEWPORT                 vp{};
    std::string                              srvNames[kSrvCount];
    float                                    cb2AtBind[kCb2VecCount * 4]   = {};
    // Deferred snapshot — captured at the *next* PSSetShader call. cb2 only;
    // the rest of the state is unlikely to change between two consecutive PS
    // binds for the same family of passes.
    float                                    cb2Deferred[kCb2VecCount * 4] = {};
    // Raw RE::Sky::directionalAmbientColorsA[3][2] snapshot. 6 NiColor values
    // (X+, X-, Y+, Y-, Z+, Z-) × 3 channels (R, G, B) = 18 floats. Captured
    // alongside cb2 so a single CSV row holds both the engine's packed
    // cb2[6..8] AND the source 6-axis ambient cube — letting the reader
    // diff the two and pin down the exact packing transform.
    float                                    dirAmbCube[6][3] = {};
    bool                                     dirAmbValid = false;
    // Camera basis vectors (world-space) captured at PSSetShader time.
    // Used to validate the camera-aligned SH packing hypothesis: the
    // engine's cb2[6..8] gradient should equal
    //   [cam_right; cam_basis_row1; cam_forward] @ (dac_diff/2)
    // for some sign/row1 convention. Logging these here lets the diff
    // post-process compute the matrix directly from current engine state.
    float                                    camRight[3]   = {};
    float                                    camUp[3]      = {};
    float                                    camForward[3] = {};
    bool                                     camValid      = false;
};
PendingCapture g_pending;

// Per-capture diagnostics. Tell us what OnDrawImpl observed during the
// frame, separate from how many rows we emitted. If totalInvocations > 0 but
// emittedRows == 0, the family filter rejected every draw — and the bucket
// sets below show which UIDs were rejected so the filter can be tightened
// or loosened. g_drawHookFires counts every Draw* hook entry seen during a
// capture window regardless of state: if it stays zero while the engine is
// rendering, the D3D11 vtable hooks aren't firing at all for these draws
// (command-buffer replay or ENB-wrapped context path).
std::uint64_t                          g_drawHookFiresAtOpen  = 0;
std::atomic<std::uint64_t>             g_drawHookFires{ 0 };
std::uint32_t                          g_diagTotalInvocations = 0;
std::uint32_t                          g_diagNullOrigPS       = 0;
std::uint32_t                          g_diagMissingDBEntry   = 0;
std::unordered_map<std::string, int>   g_diagAcceptedUIDs;
std::unordered_map<std::string, int>   g_diagRejectedUIDs;
constexpr std::size_t                  kDiagUIDLimit = 64;

bool EnsureStagingBuffer()
{
    if (g_stagingCB) return true;
    if (!g_rendererData || !g_rendererData->device) return false;

    REX::W32::D3D11_BUFFER_DESC d{};
    d.byteWidth           = kStagingByteWidth;
    d.usage               = REX::W32::D3D11_USAGE_STAGING;
    d.bindFlags           = 0;
    d.cpuAccessFlags      = REX::W32::D3D11_CPU_ACCESS_READ;
    d.miscFlags           = 0;
    d.structureByteStride = 0;

    HRESULT hr = g_rendererData->device->CreateBuffer(&d, nullptr, &g_stagingCB);
    if (FAILED(hr) || !g_stagingCB) {
        REX::WARN("LightTracker: CreateBuffer(staging) failed, hr=0x{:08X}", static_cast<std::uint32_t>(hr));
        g_stagingCB = nullptr;
        return false;
    }
    return true;
}

void ReleaseStagingBuffer()
{
    if (g_stagingCB) {
        g_stagingCB->Release();
        g_stagingCB = nullptr;
    }
}

const char* BlendStr(REX::W32::D3D11_BLEND b)
{
    switch (b) {
        case REX::W32::D3D11_BLEND_ZERO:             return "ZERO";
        case REX::W32::D3D11_BLEND_ONE:              return "ONE";
        case REX::W32::D3D11_BLEND_SRC_COLOR:        return "SRC_COLOR";
        case REX::W32::D3D11_BLEND_INV_SRC_COLOR:    return "INV_SRC_COLOR";
        case REX::W32::D3D11_BLEND_SRC_ALPHA:        return "SRC_ALPHA";
        case REX::W32::D3D11_BLEND_INV_SRC_ALPHA:    return "INV_SRC_ALPHA";
        case REX::W32::D3D11_BLEND_DEST_ALPHA:       return "DEST_ALPHA";
        case REX::W32::D3D11_BLEND_INV_DEST_ALPHA:   return "INV_DEST_ALPHA";
        case REX::W32::D3D11_BLEND_DEST_COLOR:       return "DEST_COLOR";
        case REX::W32::D3D11_BLEND_INV_DEST_COLOR:   return "INV_DEST_COLOR";
        case REX::W32::D3D11_BLEND_SRC_ALPHA_SAT:    return "SRC_ALPHA_SAT";
        case REX::W32::D3D11_BLEND_BLEND_FACTOR:     return "BLEND_FACTOR";
        case REX::W32::D3D11_BLEND_INV_BLEND_FACTOR: return "INV_BLEND_FACTOR";
        case REX::W32::D3D11_BLEND_SRC1_COLOR:       return "SRC1_COLOR";
        case REX::W32::D3D11_BLEND_INV_SRC1_COLOR:   return "INV_SRC1_COLOR";
        case REX::W32::D3D11_BLEND_SRC1_ALPHA:       return "SRC1_ALPHA";
        case REX::W32::D3D11_BLEND_INV_SRC1_ALPHA:   return "INV_SRC1_ALPHA";
        default:                                     return "?";
    }
}

const char* BlendOpStr(REX::W32::D3D11_BLEND_OP op)
{
    switch (op) {
        case REX::W32::D3D11_BLEND_OP_ADD:          return "ADD";
        case REX::W32::D3D11_BLEND_OP_SUBTRACT:     return "SUB";
        case REX::W32::D3D11_BLEND_OP_REV_SUBTRACT: return "REVSUB";
        case REX::W32::D3D11_BLEND_OP_MIN:          return "MIN";
        case REX::W32::D3D11_BLEND_OP_MAX:          return "MAX";
        default:                                    return "?";
    }
}

// Map a D3D resource back to its engine render-target / depth-stencil slot.
// Mirrors CustomPass.cpp's helper but returns a short token suitable for a
// CSV cell (e.g. "rt[3]", "ds[2]").
std::string IdentifyResource(REX::W32::ID3D11Resource* resource)
{
    if (!resource || !g_rendererData) return "(null)";
    REX::W32::ID3D11Texture2D* tex = nullptr;
    if (FAILED(resource->QueryInterface(REX::W32::IID_ID3D11Texture2D,
                                        reinterpret_cast<void**>(&tex))) || !tex) {
        return "(non2d)";
    }

    std::string out;
    for (int i = 0; i < RT::idx(RT::Color::kCount); ++i) {
        const auto& rt = g_rendererData->renderTargets[i];
        if (rt.texture == tex || rt.copyTexture == tex) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "rt[%d]%s", i, rt.copyTexture == tex ? ".cpy" : "");
            out = buf;
            break;
        }
    }
    if (out.empty()) {
        for (int i = 0; i < RT::idx(RT::Depth::kCount); ++i) {
            const auto& dt = g_rendererData->depthStencilTargets[i];
            if (dt.texture == tex) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "ds[%d]", i);
                out = buf;
                break;
            }
        }
    }
    if (out.empty()) out = "ext";
    tex->Release();
    return out;
}

std::string IdentifySRV(REX::W32::ID3D11ShaderResourceView* srv)
{
    if (!srv) return "";
    REX::W32::ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    std::string s = IdentifyResource(res);
    if (res) res->Release();
    return s;
}

struct ShaderInfo {
    bool          inFamily       = false;
    bool          hasReplacement = false;
    std::string   uid;
    std::string   definitionId;
};

ShaderInfo LookupShader(REX::W32::ID3D11PixelShader* origPS)
{
    ShaderInfo info;
    if (!origPS) return info;

    std::shared_lock lock(g_ShaderDB.mutex);
    auto it = g_ShaderDB.entries.find(origPS);
    if (it == g_ShaderDB.entries.end()) return info;

    const auto& e = it->second;
    if (e.type != ShaderType::Pixel) return info;

    info.uid = e.shaderUID;
    if (e.matchedDefinition) {
        info.definitionId   = e.matchedDefinition->id;
        info.hasReplacement = e.GetReplacementPixelShader() != nullptr;
        if (info.definitionId.rfind("pixelDeferredLight", 0) == 0) {
            info.inFamily = true;
        }
    }
    // Heuristic: unhooked deferred-light variants share a strict signature —
    // 2 MRTs (diffuse + specular accumulate), outputMask 0x3, and a cb2 byte
    // width matching one of the observed sizes (368 local / 448 main+ambient /
    // 480 local-with-shadow). Keeps the filter narrow while still surfacing
    // variants we haven't hooked yet.
    if (!info.inFamily && e.outputCount == 2 && e.outputMask == 0x3) {
        const auto cb2Size = e.expectedCBSizes[2];
        if (cb2Size == 368 || cb2Size == 448 || cb2Size == 480) {
            info.inFamily = true;
        }
    }
    return info;
}

void OpenCsv()
{
    auto dir = g_pluginPath / "LightTracker";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        REX::WARN("LightTracker: create_directories failed: {}", ec.message());
    }

    auto       now = std::chrono::system_clock::now();
    auto       t   = std::chrono::system_clock::to_time_t(now);
    std::tm    tm{};
    localtime_s(&tm, &t);
    char fname[128];
    std::snprintf(fname, sizeof(fname),
                  "lights_%04d%02d%02d_%02d%02d%02d.csv",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    g_csvPath = dir / fname;

    g_csv.open(g_csvPath, std::ios::out | std::ios::trunc);
    if (!g_csv.is_open()) {
        REX::WARN("LightTracker: failed to open {}", g_csvPath.string());
        return;
    }

    g_csv << "frame,bindIdx,uid,definition,hooked,"
             "rtv0,blendEnable,srcBlend,dstBlend,blendOp,writeMask,"
             "scissorX,scissorY,scissorW,scissorH,vpW,vpH";
    for (UINT i = 0; i < kSrvCount; ++i) g_csv << ",t" << i;
    // cb2 captured at PSSetShader time (correct for local-light passes —
    // engine commits cb2 before PSSetShader for those).
    for (int v = 0; v < kCb2VecCount; ++v) {
        g_csv << ",cb2bind_" << v << "x,cb2bind_" << v << "y,cb2bind_" << v << "z,cb2bind_" << v << "w";
    }
    // cb2 captured at the *next* PSSetShader call (correct for ambient/main
    // passes — engine commits cb2 after PSSetShader for those, so by the
    // next bind it has been written).
    for (int v = 0; v < kCb2VecCount; ++v) {
        g_csv << ",cb2def_" << v << "x,cb2def_" << v << "y,cb2def_" << v << "z,cb2def_" << v << "w";
    }
    // RE::Sky::directionalAmbientColorsA — 6 NiColor entries × 3 channels.
    // Captured per-bind so each cb2 row also has the source ambient cube.
    // Axis tags: Xp/Xn/Yp/Yn/Zp/Zn for the 6 +/- directions.
    g_csv << ",dac_valid";
    static const char* axisTags[6] = { "Xp", "Xn", "Yp", "Yn", "Zp", "Zn" };
    for (int i = 0; i < 6; ++i) {
        g_csv << ",dac_" << axisTags[i] << "_r"
              << ",dac_" << axisTags[i] << "_g"
              << ",dac_" << axisTags[i] << "_b";
    }
    // Camera basis (world-space) at PSSetShader time.
    g_csv << ",cam_valid"
          << ",cam_right_x,cam_right_y,cam_right_z"
          << ",cam_up_x,cam_up_y,cam_up_z"
          << ",cam_forward_x,cam_forward_y,cam_forward_z";
    g_csv << "\n";
    g_csv.flush();

    REX::INFO("LightTracker: capturing frame {} -> {}",
              g_frameCounter, g_csvPath.string());
}

void CloseCsv()
{
    if (g_csv.is_open()) {
        g_csv.flush();
        g_csv.close();
        REX::INFO("LightTracker: closed {} ({} rows captured)",
                  g_csvPath.string(), g_emittedRows);
        const auto absDrawHooks   = g_drawHookFires.load(std::memory_order_relaxed);
        const auto deltaDrawHooks = absDrawHooks - g_drawHookFiresAtOpen;
        REX::INFO("LightTracker: diag - drawHooksAbs={} drawHooksDelta={} invocations={} nullPS={} missingDBEntry={} acceptedUIDs={} rejectedUIDs={}",
                  absDrawHooks, deltaDrawHooks,
                  g_diagTotalInvocations, g_diagNullOrigPS, g_diagMissingDBEntry,
                  g_diagAcceptedUIDs.size(), g_diagRejectedUIDs.size());
        int n = 0;
        for (const auto& [uid, count] : g_diagAcceptedUIDs) {
            REX::INFO("LightTracker:   accepted {} x{}", uid, count);
            if (++n >= 20) break;
        }
        n = 0;
        for (const auto& [uid, count] : g_diagRejectedUIDs) {
            REX::INFO("LightTracker:   rejected {} x{}", uid, count);
            if (++n >= 20) break;
        }
    }
    g_drawIdx              = 0;
    g_emittedRows          = 0;
    g_diagTotalInvocations = 0;
    g_diagNullOrigPS       = 0;
    g_diagMissingDBEntry   = 0;
    g_diagAcceptedUIDs.clear();
    g_diagRejectedUIDs.clear();
}

}  // anonymous namespace

// Forward decl — definition is further down with the other capture helpers,
// but Tick needs to flush any in-flight pending capture at frame close.
static void FlushPending(REX::W32::ID3D11DeviceContext* ctx);

std::atomic<bool> g_isActive{ false };

void Initialize()
{
    g_state          = State::Idle;
    g_prevHotkeyDown = false;
    g_frameCounter   = 0;
    g_drawIdx        = 0;
    g_emittedRows    = 0;
    // In DEVELOPMENT we keep the hot path on at all times so the inline
    // OnDraw call lands in OnDrawImpl every frame. OnDrawImpl handles the
    // state-machine + family checks itself, and unconditionally bumps the
    // diagnostic draw counter — that's what tells us whether the hook fires
    // for the deferred-light passes at all.
    g_isActive.store(DEVELOPMENT, std::memory_order_release);
    REX::INFO("LightTracker: initialized (hotkey: Numpad *, DEVELOPMENT={})",
              DEVELOPMENT ? "on" : "off");
}

void Shutdown()
{
    g_isActive.store(false, std::memory_order_release);
    g_state = State::Idle;
    if (g_csv.is_open()) {
        g_csv.flush();
        g_csv.close();
    }
    ReleaseStagingBuffer();
}

void Tick()
{
    if (!DEVELOPMENT) return;

    // Finalize any frame whose draws have just completed. MyPresent fires
    // after the engine has submitted all of frame N's draws, so by the time
    // we re-enter Tick (start of MyPresent for frame N+1), the capture is
    // done and the file should be closed.
    if (g_state == State::Capturing) {
        // Flush the last in-flight pending capture using whatever cb2 is
        // currently bound. By this point the engine has finished its draws
        // for the captured frame so the bound cb2 is the closing state for
        // the last pass we saw.
        if (g_pending.valid && g_rendererData && g_rendererData->context) {
            FlushPending(g_rendererData->context);
        }
        g_pending = {};
        CloseCsv();
        g_state = State::Idle;
        // Hot path stays on while DEVELOPMENT is set so the diagnostic
        // counter keeps tracking — it just won't write CSV rows.
    }

    const bool down   = (GetAsyncKeyState(kHotkey) & 0x8000) != 0;
    const bool rising = down && !g_prevHotkeyDown;
    g_prevHotkeyDown  = down;
    if (rising && g_state == State::Idle) {
        g_state = State::Armed;
        REX::INFO("LightTracker: armed; next frame will be captured");
    }

    ++g_frameCounter;

    // Promote Armed → Capturing for the upcoming frame's draws. We open the
    // file *now* (before any draws of the captured frame run) so OnDrawImpl
    // can append rows without any per-draw open/close overhead.
    if (g_state == State::Armed) {
        OpenCsv();
        if (g_csv.is_open()) {
            g_state = State::Capturing;
            // Snapshot the unconditional draw-hook counter so CloseCsv can
            // report the delta — i.e. "how many draws fired between Open
            // and Close, regardless of family or state". A delta of zero is
            // the smoking gun for command-buffer replay paths.
            g_drawHookFiresAtOpen = g_drawHookFires.load(std::memory_order_relaxed);
        } else {
            g_state = State::Idle;  // file open failed; bail.
        }
    }
}

// Read cb2 (slot 2 of the PS constant-buffer table) into `out`. Returns the
// number of bytes actually copied. Used by both the at-bind snapshot and the
// deferred snapshot.
static UINT ReadCB2(REX::W32::ID3D11DeviceContext* ctx, float* out, std::size_t outBytes)
{
    REX::W32::ID3D11Buffer* cb2Buffer = nullptr;
    ctx->PSGetConstantBuffers(2, 1, &cb2Buffer);
    if (!cb2Buffer || !EnsureStagingBuffer()) {
        if (cb2Buffer) cb2Buffer->Release();
        return 0;
    }
    REX::W32::D3D11_BUFFER_DESC d{};
    cb2Buffer->GetDesc(&d);
    const UINT bytes = std::min<UINT>(d.byteWidth, static_cast<UINT>(outBytes));
    REX::W32::D3D11_BOX box{ 0, 0, 0, bytes, 1, 1 };
    ctx->CopySubresourceRegion(g_stagingCB, 0, 0, 0, 0, cb2Buffer, 0, &box);
    UINT copied = 0;
    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(g_stagingCB, 0, REX::W32::D3D11_MAP_READ, 0, &mapped))) {
        std::memcpy(out, mapped.data, bytes);
        ctx->Unmap(g_stagingCB, 0);
        copied = bytes;
    }
    cb2Buffer->Release();
    return copied;
}

static void FlushPending(REX::W32::ID3D11DeviceContext* ctx)
{
    if (!g_pending.valid || !g_csv.is_open()) return;

    // Deferred cb2 read — this is what the engine has bound *now*, after the
    // pending pass's draw completed. For passes that pre-bind cb2 it's
    // already been overwritten by the new pass's setup; for passes that
    // post-bind cb2 this is the correct read.
    if (ctx) {
        ReadCB2(ctx, g_pending.cb2Deferred, sizeof(g_pending.cb2Deferred));
    }

    g_csv << g_pending.frame << ','
          << g_pending.bindIdx << ','
          << g_pending.uid << ','
          << g_pending.definitionId << ','
          << (g_pending.hasReplacement ? '1' : '0') << ','
          << g_pending.rtvName << ','
          << (g_pending.rt0Blend.blendEnable ? '1' : '0') << ','
          << BlendStr(g_pending.rt0Blend.srcBlend)  << ','
          << BlendStr(g_pending.rt0Blend.destBlend) << ','
          << BlendOpStr(g_pending.rt0Blend.blendOp) << ','
          << static_cast<std::uint32_t>(g_pending.rt0Blend.renderTargetWriteMask) << ','
          << g_pending.scissor.x1 << ',' << g_pending.scissor.y1 << ','
          << (g_pending.scissor.x2 - g_pending.scissor.x1) << ','
          << (g_pending.scissor.y2 - g_pending.scissor.y1) << ','
          << g_pending.vp.width << ',' << g_pending.vp.height;
    for (UINT i = 0; i < kSrvCount; ++i) g_csv << ',' << g_pending.srvNames[i];
    for (int v = 0; v < kCb2VecCount; ++v) {
        g_csv << ',' << g_pending.cb2AtBind[v * 4 + 0]
              << ',' << g_pending.cb2AtBind[v * 4 + 1]
              << ',' << g_pending.cb2AtBind[v * 4 + 2]
              << ',' << g_pending.cb2AtBind[v * 4 + 3];
    }
    for (int v = 0; v < kCb2VecCount; ++v) {
        g_csv << ',' << g_pending.cb2Deferred[v * 4 + 0]
              << ',' << g_pending.cb2Deferred[v * 4 + 1]
              << ',' << g_pending.cb2Deferred[v * 4 + 2]
              << ',' << g_pending.cb2Deferred[v * 4 + 3];
    }
    g_csv << ',' << (g_pending.dirAmbValid ? '1' : '0');
    for (int i = 0; i < 6; ++i) {
        g_csv << ',' << g_pending.dirAmbCube[i][0]
              << ',' << g_pending.dirAmbCube[i][1]
              << ',' << g_pending.dirAmbCube[i][2];
    }
    g_csv << ',' << (g_pending.camValid ? '1' : '0')
          << ',' << g_pending.camRight[0]   << ',' << g_pending.camRight[1]   << ',' << g_pending.camRight[2]
          << ',' << g_pending.camUp[0]      << ',' << g_pending.camUp[1]      << ',' << g_pending.camUp[2]
          << ',' << g_pending.camForward[0] << ',' << g_pending.camForward[1] << ',' << g_pending.camForward[2];
    g_csv << '\n';
    ++g_emittedRows;
    if ((g_emittedRows & 0xFF) == 0) g_csv.flush();

    g_pending = {};
}

void OnPSBindImpl(REX::W32::ID3D11DeviceContext* ctx,
                  REX::W32::ID3D11PixelShader*   origPS)
{
    g_drawHookFires.fetch_add(1, std::memory_order_relaxed);

    if (g_state != State::Capturing || !g_csv.is_open() || !ctx) return;

    // Flush any pending capture FIRST. Whatever cb2 is currently bound is
    // what the engine left committed at the end of the previous pass — for
    // ambient/main shaders that post-bind cb2, this is the correct read.
    FlushPending(ctx);

    ++g_diagTotalInvocations;

    if (!origPS) {
        ++g_diagNullOrigPS;
        ++g_drawIdx;
        return;
    }
    const ShaderInfo info = LookupShader(origPS);
    if (info.uid.empty()) ++g_diagMissingDBEntry;

    const std::string key = !info.uid.empty() ? info.uid : std::string("(unknown)");
    if (info.inFamily) {
        if (g_diagAcceptedUIDs.size() < kDiagUIDLimit || g_diagAcceptedUIDs.count(key))
            g_diagAcceptedUIDs[key]++;
    } else {
        if (g_diagRejectedUIDs.size() < kDiagUIDLimit || g_diagRejectedUIDs.count(key))
            g_diagRejectedUIDs[key]++;
    }

    if (!info.inFamily) {
        ++g_drawIdx;
        return;
    }

    // Build the pending-capture record. State we capture *now* is correct
    // for passes that pre-bind everything (local lights). The deferred cb2
    // re-read at the next PSSetShader covers passes that post-bind cb2.

    g_pending.valid          = true;
    g_pending.uid            = info.uid;
    g_pending.definitionId   = info.definitionId;
    g_pending.hasReplacement = info.hasReplacement;
    g_pending.frame          = g_frameCounter;
    g_pending.bindIdx        = g_drawIdx++;

    REX::W32::ID3D11RenderTargetView* rtv0 = nullptr;
    REX::W32::ID3D11DepthStencilView* dsv  = nullptr;
    ctx->OMGetRenderTargets(1, &rtv0, &dsv);
    if (dsv) dsv->Release();
    g_pending.rtvName = "(none)";
    if (rtv0) {
        REX::W32::ID3D11Resource* res = nullptr;
        rtv0->GetResource(&res);
        g_pending.rtvName = IdentifyResource(res);
        if (res) res->Release();
    }

    REX::W32::ID3D11BlendState* bs    = nullptr;
    float                       bf[4] = {};
    UINT                        sm    = 0;
    ctx->OMGetBlendState(&bs, bf, &sm);
    g_pending.rt0Blend = {};
    if (bs) {
        REX::W32::D3D11_BLEND_DESC bd{};
        bs->GetDesc(&bd);
        g_pending.rt0Blend = bd.renderTarget[0];
    }

    UINT numScissors = 1;
    ctx->RSGetScissorRects(&numScissors, &g_pending.scissor);
    UINT numVp = 1;
    ctx->RSGetViewports(&numVp, &g_pending.vp);

    REX::W32::ID3D11ShaderResourceView* srvs[kSrvCount] = {};
    ctx->PSGetShaderResources(0, kSrvCount, srvs);
    for (UINT i = 0; i < kSrvCount; ++i) g_pending.srvNames[i] = IdentifySRV(srvs[i]);

    std::memset(g_pending.cb2AtBind, 0, sizeof(g_pending.cb2AtBind));
    std::memset(g_pending.cb2Deferred, 0, sizeof(g_pending.cb2Deferred));
    ReadCB2(ctx, g_pending.cb2AtBind, sizeof(g_pending.cb2AtBind));

    // Snapshot RE::Sky::directionalAmbientColorsA. Per-bind read is cheap
    // (just three pointer dereferences + 18 float copies), and lets the CSV
    // capture both the cb2 and the source ambient cube atomically.
    g_pending.dirAmbValid = false;
    std::memset(g_pending.dirAmbCube, 0, sizeof(g_pending.dirAmbCube));
    if (auto* sky = RE::Sky::GetSingleton()) {
        const auto& dac = sky->directionalAmbientColorsA;
        // dac[axis][sign]: axis 0=X, 1=Y, 2=Z; sign 0=+, 1=-.
        // CSV order is Xp, Xn, Yp, Yn, Zp, Zn (matches axisTags in OpenCsv).
        const RE::NiColor srcs[6] = {
            dac[0][0], dac[0][1],
            dac[1][0], dac[1][1],
            dac[2][0], dac[2][1],
        };
        for (int i = 0; i < 6; ++i) {
            g_pending.dirAmbCube[i][0] = srcs[i].r;
            g_pending.dirAmbCube[i][1] = srcs[i].g;
            g_pending.dirAmbCube[i][2] = srcs[i].b;
        }
        g_pending.dirAmbValid = true;
    }

    // Snapshot camera basis at PSSetShader time. The SH packing hypothesis
    // is that cb2.xyz bands come from [camera_right, ?, camera_forward]
    // applied to the dac gradient. Logging the basis here lets us verify
    // the row-1 convention (-camera_up? +camera_up? something else?) by
    // comparing captured cb2 with the predicted M @ gradient.
    g_pending.camValid = false;
    std::memset(g_pending.camRight,   0, sizeof(g_pending.camRight));
    std::memset(g_pending.camUp,      0, sizeof(g_pending.camUp));
    std::memset(g_pending.camForward, 0, sizeof(g_pending.camForward));
    if (g_rendererData) {
        // The default gfxState.cameraState is STALE for gameplay — it holds
        // some early-init camera basis that never updates. The gameplay
        // camera lives in gfxState.cameraDataCache, keyed by the world
        // camera node. Match Plugin.cpp's SelectGameplayCameraState logic.
        auto gfxState = RE::BSGraphics::State::GetSingleton();
        const RE::BSGraphics::CameraStateData* camState = nullptr;
        const auto* worldCamera = RE::Main::WorldRootCamera();
        if (worldCamera) {
            for (const auto& cached : gfxState.cameraDataCache) {
                if (cached.referenceCamera == worldCamera) {
                    camState = std::addressof(cached);
                    break;
                }
            }
        }
        if (!camState) {
            camState = std::addressof(gfxState.cameraState);
        }
        const auto& camView = camState->camViewData;
        g_pending.camRight[0]   = camView.viewRight.m128_f32[0];
        g_pending.camRight[1]   = camView.viewRight.m128_f32[1];
        g_pending.camRight[2]   = camView.viewRight.m128_f32[2];
        g_pending.camUp[0]      = camView.viewUp.m128_f32[0];
        g_pending.camUp[1]      = camView.viewUp.m128_f32[1];
        g_pending.camUp[2]      = camView.viewUp.m128_f32[2];
        g_pending.camForward[0] = camView.viewDir.m128_f32[0];
        g_pending.camForward[1] = camView.viewDir.m128_f32[1];
        g_pending.camForward[2] = camView.viewDir.m128_f32[2];
        g_pending.camValid = true;
    }

    if (rtv0) rtv0->Release();
    if (bs)   bs->Release();
    for (UINT i = 0; i < kSrvCount; ++i) {
        if (srvs[i]) srvs[i]->Release();
    }
}

}  // namespace LightTracker
