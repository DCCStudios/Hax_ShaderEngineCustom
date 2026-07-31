#include <ShadowMapDeferredLighting.h>

#include <Global.h>
#include <Plugin.h>
#include <RenderTargets.h>
#include <ShaderResources.h>
#include <ShadowUpgrade.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ShadowMapDeferredLighting {
namespace {

enum class Contract : std::uint32_t
{
    kPlanarSingle = 0,
    kPlanarGrid3 = 1,
    kPlanarDisk16 = 2,
    kPointSingle = 3,
    kPointGrid3 = 4,
    kPointDisk16 = 5,
    kDirectionalLighting = 6,
    kDirectionalVisibility = 7,
    kCount
};

// BSDFLightShaderMacros flags recovered from the game's Get() macro emitter.
// BSDFLightShader::SetupTechnique normalizes a pass technique with
// GetPixelShaderID(), then BSShader::BeginTechnique looks that ID up in the
// pixel-shader map loaded from the DFLight section of Shaders011.fxp.
constexpr std::uint32_t kDirectionalShadow = 1u << 1;
constexpr std::uint32_t kPointOmniShadow = 1u << 3;
constexpr std::uint32_t kPointHalfShadow = 1u << 4;
constexpr std::uint32_t kPointSpotShadow = 1u << 5;
constexpr std::uint32_t kShadowOnly = 1u << 18;
constexpr std::uint32_t kFilterPcf1 = 1u << 19;
constexpr std::uint32_t kFilterPcf9 = 1u << 20;
constexpr std::uint32_t kFilterPoisson = 1u << 21;
constexpr std::uint32_t kLocalShadowMask =
    kPointOmniShadow | kPointHalfShadow | kPointSpotShadow;
constexpr std::uint32_t kSupportedFilterMask =
    kFilterPcf1 | kFilterPcf9 | kFilterPoisson;

std::uint32_t GetPixelShaderID(std::uint32_t techniqueID) noexcept
{
    // Byte-for-byte equivalent to
    // BSDFLightShaderMacros::GetPixelShaderID @ 0x1428DBFC0 (FO4 1.10.163).
    if ((techniqueID & 0x140u) != 0) {
        techniqueID &= 0xF801257Fu;
    }
    if ((techniqueID & 0x4003Au) == 0) {
        techniqueID &= 0xFC07FF7Fu;
    }
    return techniqueID;
}

std::optional<Contract> ClassifyPixelShaderID(
    std::uint32_t pixelShaderID) noexcept
{
    if ((pixelShaderID & kDirectionalShadow) != 0) {
        // The current directional processor reproduces Bethesda's 16-tap
        // Poisson denominator. Other FXP filter families remain vanilla until
        // their exact constant layouts are implemented.
        if ((pixelShaderID & kSupportedFilterMask) !=
            kFilterPoisson) {
            return std::nullopt;
        }
        return (pixelShaderID & kShadowOnly) != 0 ?
            Contract::kDirectionalVisibility :
            Contract::kDirectionalLighting;
    }

    const auto localShadow = pixelShaderID & kLocalShadowMask;
    const auto filter = pixelShaderID & kSupportedFilterMask;
    std::uint32_t filterIndex = 0;
    if (filter == kFilterPcf1) {
        filterIndex = 0;
    } else if (filter == kFilterPcf9) {
        filterIndex = 1;
    } else if (filter == kFilterPoisson) {
        filterIndex = 2;
    } else {
        return std::nullopt;
    }

    if ((localShadow & kPointSpotShadow) != 0) {
        return static_cast<Contract>(
            static_cast<std::uint32_t>(Contract::kPlanarSingle) +
            filterIndex);
    }
    if ((localShadow &
            (kPointOmniShadow | kPointHalfShadow)) != 0) {
        return static_cast<Contract>(
            static_cast<std::uint32_t>(Contract::kPointSingle) +
            filterIndex);
    }
    return std::nullopt;
}

constexpr UINT kScratchSrvSlot = 26;
constexpr UINT kScratchTargetCount = 4;
constexpr UINT kShadowedScratchIndex = 2;
constexpr UINT kModeConstantSlot = 13;
constexpr std::array<const char*, 2> kShaderFiles{
    "ShadowMapDeferredLighting.hlsl",
    "DirectionalShadowMapDeferredLighting.hlsl"
};

enum class ShaderKind : std::size_t
{
    kLocal = 0,
    kDirectional = 1,
    kCount
};

struct ClassifiedContract
{
    Contract contract = Contract::kPlanarSingle;
    std::uint32_t techniqueID = 0;
    std::uint32_t pixelShaderID = 0;
    std::uint32_t textureSlotMask = 0;
    std::array<std::uint32_t, 14> cbSizes{};
    int outputCount = 0;

    [[nodiscard]] bool IsDirectional() const noexcept
    {
        return contract == Contract::kDirectionalLighting ||
            contract == Contract::kDirectionalVisibility;
    }
};

struct ObservedSelection
{
    Contract contract = Contract::kPlanarSingle;
    std::uint32_t techniqueID = 0;
    std::uint32_t pixelShaderID = 0;
};

template <class T>
void Release(T*& value) noexcept
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

struct ScratchTarget
{
    REX::W32::ID3D11Texture2D* texture = nullptr;
    REX::W32::ID3D11RenderTargetView* rtv = nullptr;
    REX::W32::ID3D11ShaderResourceView* srv = nullptr;
    REX::W32::D3D11_TEXTURE2D_DESC sourceDesc{};

    void Reset() noexcept
    {
        Release(srv);
        Release(rtv);
        Release(texture);
        sourceDesc = {};
    }
};

struct ReplayDepthStencilState
{
    // Retaining the source prevents an ID3D11DepthStencilState pointer from
    // being recycled while its write-suppressed clone is cached.
    REX::W32::ID3D11DepthStencilState* source = nullptr;
    REX::W32::ID3D11DepthStencilState* replay = nullptr;
};

struct SavedState
{
    REX::W32::ID3D11DeviceContext* context = nullptr;
    std::array<REX::W32::ID3D11RenderTargetView*, 8> rtvs{};
    REX::W32::ID3D11DepthStencilView* dsv = nullptr;
    REX::W32::ID3D11BlendState* blend = nullptr;
    float blendFactor[4]{};
    UINT sampleMask = 0xFFFFFFFFu;
    REX::W32::ID3D11DepthStencilState* depthStencil = nullptr;
    UINT stencilRef = 0;
    REX::W32::ID3D11PixelShader* pixelShader = nullptr;
    std::array<
        REX::W32::ID3D11ShaderResourceView*,
        kScratchTargetCount> scratchSlots{};
    std::array<REX::W32::ID3D11ShaderResourceView*, 3> modularSlots{};
    REX::W32::ID3D11Buffer* modeConstant = nullptr;
    REX::W32::ID3D11SamplerState* shadowSampler = nullptr;
    bool captured = false;
    bool modified = false;

    explicit SavedState(REX::W32::ID3D11DeviceContext* a_context) noexcept :
        context(a_context)
    {
        if (!context) {
            return;
        }
        context->OMGetRenderTargets(
            static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
        context->OMGetBlendState(&blend, blendFactor, &sampleMask);
        context->OMGetDepthStencilState(&depthStencil, &stencilRef);
        context->PSGetShader(&pixelShader, nullptr, nullptr);
        context->PSGetShaderResources(
            kScratchSrvSlot,
            static_cast<UINT>(scratchSlots.size()),
            scratchSlots.data());
        context->PSGetShaderResources(
            MODULAR_FLOATS_SLOT, 1, &modularSlots[0]);
        context->PSGetShaderResources(
            MODULAR_INTS_SLOT, 1, &modularSlots[1]);
        context->PSGetShaderResources(
            MODULAR_BOOLS_SLOT, 1, &modularSlots[2]);
        context->PSGetConstantBuffers(
            kModeConstantSlot, 1, &modeConstant);
        context->PSGetSamplers(5, 1, &shadowSampler);
        captured = true;
    }

    ~SavedState()
    {
        Restore();
        for (auto*& rtv : rtvs) {
            Release(rtv);
        }
        Release(dsv);
        Release(blend);
        Release(depthStencil);
        Release(pixelShader);
        for (auto*& srv : scratchSlots) {
            Release(srv);
        }
        for (auto*& srv : modularSlots) {
            Release(srv);
        }
        Release(modeConstant);
        Release(shadowSampler);
    }

    void MarkModified() noexcept
    {
        modified = captured;
    }

    void Restore() noexcept
    {
        if (!modified || !context) {
            return;
        }
        context->PSSetShaderResources(
            kScratchSrvSlot,
            static_cast<UINT>(scratchSlots.size()),
            scratchSlots.data());
        context->PSSetShaderResources(
            MODULAR_FLOATS_SLOT, 1, &modularSlots[0]);
        context->PSSetShaderResources(
            MODULAR_INTS_SLOT, 1, &modularSlots[1]);
        context->PSSetShaderResources(
            MODULAR_BOOLS_SLOT, 1, &modularSlots[2]);
        context->PSSetConstantBuffers(
            kModeConstantSlot, 1, &modeConstant);
        context->PSSetSamplers(5, 1, &shadowSampler);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->OMSetDepthStencilState(depthStencil, stencilRef);
        context->OMSetBlendState(blend, blendFactor, sampleMask);
        context->OMSetRenderTargets(
            static_cast<UINT>(rtvs.size()), rtvs.data(), dsv);
        modified = false;
    }
};

struct ScopedCustomDraw
{
    bool previous = false;

    ScopedCustomDraw() noexcept :
        previous(g_customPassRendering)
    {
        g_customPassRendering = true;
    }

    ~ScopedCustomDraw()
    {
        g_customPassRendering = previous;
    }
};

std::array<ScratchTarget, kScratchTargetCount> s_scratch{};
std::array<REX::W32::ID3D11PixelShader*,
           static_cast<std::size_t>(ShaderKind::kCount)> s_filterShaders{};
REX::W32::ID3D11BlendState* s_opaqueBlend = nullptr;
REX::W32::ID3D11Buffer* s_modeConstant = nullptr;
REX::W32::ID3D11SamplerState* s_alwaysVisibleSampler = nullptr;
REX::W32::ID3D11SamplerState* s_alwaysVisibleSourceSampler = nullptr;
std::vector<ReplayDepthStencilState> s_replayDepthStencilStates;
std::array<bool, static_cast<std::size_t>(ShaderKind::kCount)> s_compileTried{};
std::array<bool, static_cast<std::size_t>(ShaderKind::kCount)> s_compileFailed{};
std::unordered_map<REX::W32::ID3D11PixelShader*, ObservedSelection>
    s_observedSelections;
std::shared_mutex s_selectionMutex;
std::unordered_set<std::uint64_t> s_acceptedSelections;
std::unordered_set<std::uint64_t> s_rejectedSelections;
std::mutex s_diagnosticMutex;
std::mutex s_depthStencilMutex;
bool s_slotConflictLogged = false;

std::uint64_t SelectionKey(const ClassifiedContract& contract) noexcept
{
    return
        (static_cast<std::uint64_t>(contract.pixelShaderID) << 32) |
        contract.techniqueID;
}

bool SameScratchDesc(
    const REX::W32::D3D11_TEXTURE2D_DESC& a,
    const REX::W32::D3D11_TEXTURE2D_DESC& b) noexcept
{
    return a.width == b.width &&
        a.height == b.height &&
        a.mipLevels == b.mipLevels &&
        a.arraySize == b.arraySize &&
        a.format == b.format &&
        a.sampleDesc.count == b.sampleDesc.count &&
        a.sampleDesc.quality == b.sampleDesc.quality;
}

std::optional<ClassifiedContract> Classify(
    REX::W32::ID3D11PixelShader* originalShader,
    REX::W32::ID3D11PixelShader* activeShader) noexcept
{
    if (!originalShader || !activeShader) {
        return std::nullopt;
    }

    ObservedSelection selection{};
    {
        std::shared_lock selectionLock(s_selectionMutex);
        const auto selectionIt =
            s_observedSelections.find(originalShader);
        if (selectionIt == s_observedSelections.end()) {
            return std::nullopt;
        }
        selection = selectionIt->second;
    }

    std::uint32_t textureSlotMask = 0;
    std::array<std::uint32_t, 14> cbSizes{};
    int outputCount = 0;
    REX::W32::ID3D11PixelShader* replacementShader = nullptr;
    {
        std::shared_lock lock(g_ShaderDB.mutex);
        const auto it = g_ShaderDB.entries.find(originalShader);
        if (it == g_ShaderDB.entries.end()) {
            return std::nullopt;
        }
        textureSlotMask = it->second.textureSlotMask;
        std::copy(
            std::begin(it->second.expectedCBSizes),
            std::end(it->second.expectedCBSizes),
            cbSizes.begin());
        outputCount = it->second.outputCount;
        replacementShader =
            it->second.replacementPixelShader.load(std::memory_order_acquire);
    }

    // The PSSetShader hook records the engine object before replacement.
    // Accept only that object or the replacement published for that exact DB
    // entry. This prevents a stale atomic identity from consuming another
    // pass whose actual shader happens to remain bound.
    if (activeShader != originalShader &&
        activeShader != replacementShader) {
        return std::nullopt;
    }

    return ClassifiedContract{
        .contract = selection.contract,
        .techniqueID = selection.techniqueID,
        .pixelShaderID = selection.pixelShaderID,
        .textureSlotMask = textureSlotMask,
        .cbSizes = cbSizes,
        .outputCount = outputCount,
    };
}

bool ScratchSlotsAreAvailable() noexcept
{
    constexpr std::array<UINT, kScratchTargetCount> scratch{
        kScratchSrvSlot,
        kScratchSrvSlot + 1,
        kScratchSrvSlot + 2,
        kScratchSrvSlot + 3
    };
    const std::array<UINT, 5> injected{
        CUSTOMBUFFER_SLOT,
        DRAWTAG_SLOT,
        MODULAR_FLOATS_SLOT,
        MODULAR_INTS_SLOT,
        MODULAR_BOOLS_SLOT
    };
    for (const auto scratchSlot : scratch) {
        if (std::find(injected.begin(), injected.end(), scratchSlot) !=
            injected.end()) {
            return false;
        }
    }
    return true;
}

bool TracedDeferredLightingEnabled() noexcept
{
    for (auto* value : g_shaderSettings.GetBoolShaderValues()) {
        if (value &&
            value->id == "ps_TracedDeferredLightingEnabled") {
            return value->current.b;
        }
    }
    return true;
}

std::string AssembleShaderSource(std::string_view body)
{
    std::string source = GetCommonShaderHeaderHLSLTop();
    source += GetCommonShaderHeaderHLSLBottom();

    constexpr std::array<std::string_view, 4> component{
        ".x", ".y", ".z", ".w"
    };
    for (auto* value : g_shaderSettings.GetFloatShaderValues()) {
        source += std::format(
            "#define {} GFXModularFloats[{}]{}\n",
            value->id,
            value->bufferIndex / 4,
            component[value->bufferIndex % 4]);
    }
    for (auto* value : g_shaderSettings.GetIntShaderValues()) {
        source += std::format(
            "#define {} GFXModularInts[{}]{}\n",
            value->id,
            value->bufferIndex / 4,
            component[value->bufferIndex % 4]);
    }
    for (auto* value : g_shaderSettings.GetBoolShaderValues()) {
        source += std::format(
            "#define {} (GFXModularBools[{}]{} != 0)\n",
            value->id,
            value->bufferIndex / 4,
            component[value->bufferIndex % 4]);
    }
    source.push_back('\n');
    source.append(body.data(), body.size());
    return source;
}

bool EnsureShader(
    REX::W32::ID3D11Device* device,
    ShaderKind kind)
{
    const auto index = static_cast<std::size_t>(kind);
    auto*& shader = s_filterShaders[index];
    if (shader) {
        return true;
    }
    if (s_compileFailed[index] || s_compileTried[index] || !device) {
        return false;
    }
    s_compileTried[index] = true;

    const auto path = g_commonShaderHeaderPath / kShaderFiles[index];
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        REX::WARN(
            "ShadowMapDeferredLighting: shader file not found: {}",
            path.string());
        s_compileFailed[index] = true;
        return false;
    }
    const std::string body{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };
    const std::string source = AssembleShaderSource(body);

    constexpr std::uint32_t kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const std::string cacheKey = ShaderCache::ComputeKey({
        .assembledSource = source,
        .profile = "ps_5_0",
        .entry = "main",
        .flags = kCompileFlags,
    });

    ID3DBlob* blob = nullptr;
    if (!ShaderCache::TryLoad(cacheKey, &blob)) {
        ID3DBlob* errors = nullptr;
        auto* includeHandler = new ShaderIncludeHandler();
        const HRESULT hr = D3DCompile(
            source.data(),
            source.size(),
            kShaderFiles[index],
            nullptr,
            includeHandler,
            "main",
            "ps_5_0",
            kCompileFlags,
            0,
            &blob,
            &errors);
        delete includeHandler;

        if (!REX::W32::SUCCESS(hr) || !blob) {
            if (errors) {
                REX::WARN(
                    "ShadowMapDeferredLighting: compile failed: {}",
                    static_cast<const char*>(errors->GetBufferPointer()));
            } else {
                REX::WARN(
                    "ShadowMapDeferredLighting: compile failed 0x{:08X}",
                    static_cast<unsigned>(hr));
            }
            Release(errors);
            Release(blob);
            s_compileFailed[index] = true;
            return false;
        }
        Release(errors);
        ShaderCache::Store(cacheKey, blob);
    }

    const bool previousCreationFlag = g_isCreatingReplacementShader;
    g_isCreatingReplacementShader = true;
    const HRESULT hr = device->CreatePixelShader(
        blob->GetBufferPointer(),
        blob->GetBufferSize(),
        nullptr,
        &shader);
    g_isCreatingReplacementShader = previousCreationFlag;
    const auto byteCount = blob->GetBufferSize();
    Release(blob);

    if (!REX::W32::SUCCESS(hr) || !shader) {
        REX::WARN(
            "ShadowMapDeferredLighting: CreatePixelShader failed 0x{:08X}",
            static_cast<unsigned>(hr));
        s_compileFailed[index] = true;
        return false;
    }

    REX::INFO(
        "ShadowMapDeferredLighting: compiled {} map-space PCSS composite "
        "({} bytes)",
        kind == ShaderKind::kDirectional ? "directional" : "local",
        byteCount);
    return true;
}

bool EnsureFixedStates(REX::W32::ID3D11Device* device)
{
    if (!device) {
        return false;
    }

    HRESULT hr = S_OK;
    if (!s_opaqueBlend) {
        REX::W32::D3D11_BLEND_DESC desc{};
        desc.renderTarget[0].renderTargetWriteMask =
            REX::W32::D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &s_opaqueBlend);
        if (!REX::W32::SUCCESS(hr)) {
            REX::WARN(
                "ShadowMapDeferredLighting: opaque blend creation failed "
                "0x{:08X}",
                static_cast<unsigned>(hr));
            return false;
        }
    }

    if (!s_modeConstant) {
        REX::W32::D3D11_BUFFER_DESC desc{};
        desc.byteWidth = 32;
        desc.usage = REX::W32::D3D11_USAGE_DYNAMIC;
        desc.bindFlags = REX::W32::D3D11_BIND_CONSTANT_BUFFER;
        desc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&desc, nullptr, &s_modeConstant);
        if (!REX::W32::SUCCESS(hr) || !s_modeConstant) {
            REX::WARN(
                "ShadowMapDeferredLighting: projection constant creation "
                "failed 0x{:08X}",
                static_cast<unsigned>(hr));
            Release(s_modeConstant);
            return false;
        }
    }
    return true;
}

void GetDepthStencilDesc(
    REX::W32::ID3D11DepthStencilState* state,
    REX::W32::D3D11_DEPTH_STENCIL_DESC& desc) noexcept
{
    if (state) {
        state->GetDesc(&desc);
        return;
    }

    // OMGetDepthStencilState returns null for D3D11's default state. Recreate
    // that descriptor explicitly before suppressing its depth writes.
    desc.depthEnable = true;
    desc.depthWriteMask = REX::W32::D3D11_DEPTH_WRITE_MASK_ALL;
    desc.depthFunc = REX::W32::D3D11_COMPARISON_LESS;
    desc.stencilEnable = false;
    desc.stencilReadMask = 0xFF;
    desc.stencilWriteMask = 0xFF;
    desc.frontFace.stencilFailOp = REX::W32::D3D11_STENCIL_OP_KEEP;
    desc.frontFace.stencilDepthFailOp = REX::W32::D3D11_STENCIL_OP_KEEP;
    desc.frontFace.stencilPassOp = REX::W32::D3D11_STENCIL_OP_KEEP;
    desc.frontFace.stencilFunc = REX::W32::D3D11_COMPARISON_ALWAYS;
    desc.backFace = desc.frontFace;
}

bool DepthStencilWrites(
    const REX::W32::D3D11_DEPTH_STENCIL_DESC& desc,
    bool& depthWrites,
    bool& stencilWrites) noexcept
{
    depthWrites =
        desc.depthEnable &&
        desc.depthWriteMask != REX::W32::D3D11_DEPTH_WRITE_MASK_ZERO;

    const auto mutates =
        [](const REX::W32::D3D11_DEPTH_STENCILOP_DESC& face) {
            return
                face.stencilFailOp != REX::W32::D3D11_STENCIL_OP_KEEP ||
                face.stencilDepthFailOp != REX::W32::D3D11_STENCIL_OP_KEEP ||
                face.stencilPassOp != REX::W32::D3D11_STENCIL_OP_KEEP;
        };
    stencilWrites =
        desc.stencilEnable &&
        desc.stencilWriteMask != 0 &&
        (mutates(desc.frontFace) || mutates(desc.backFace));
    return depthWrites || stencilWrites;
}

bool EnsureWriteSuppressedDepthStencilState(
    REX::W32::ID3D11Device* device,
    REX::W32::ID3D11DepthStencilView* dsv,
    REX::W32::ID3D11DepthStencilState* source,
    REX::W32::ID3D11DepthStencilState*& replay,
    bool& depthWrites,
    bool& stencilWrites)
{
    replay = source;
    depthWrites = false;
    stencilWrites = false;
    if (!dsv) {
        return true;
    }

    REX::W32::D3D11_DEPTH_STENCIL_DESC desc{};
    GetDepthStencilDesc(source, desc);
    if (!DepthStencilWrites(desc, depthWrites, stencilWrites)) {
        return true;
    }
    if (!device) {
        replay = nullptr;
        return false;
    }

    std::lock_guard lock(s_depthStencilMutex);
    const auto cached = std::find_if(
        s_replayDepthStencilStates.begin(),
        s_replayDepthStencilStates.end(),
        [source](const ReplayDepthStencilState& value) {
            return value.source == source;
        });
    if (cached != s_replayDepthStencilStates.end()) {
        replay = cached->replay;
        return replay != nullptr;
    }

    desc.depthWriteMask = REX::W32::D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.stencilWriteMask = 0;
    desc.frontFace.stencilFailOp = REX::W32::D3D11_STENCIL_OP_KEEP;
    desc.frontFace.stencilDepthFailOp = REX::W32::D3D11_STENCIL_OP_KEEP;
    desc.frontFace.stencilPassOp = REX::W32::D3D11_STENCIL_OP_KEEP;
    desc.backFace.stencilFailOp = REX::W32::D3D11_STENCIL_OP_KEEP;
    desc.backFace.stencilDepthFailOp = REX::W32::D3D11_STENCIL_OP_KEEP;
    desc.backFace.stencilPassOp = REX::W32::D3D11_STENCIL_OP_KEEP;

    REX::W32::ID3D11DepthStencilState* created = nullptr;
    const HRESULT hr = device->CreateDepthStencilState(&desc, &created);
    if (!REX::W32::SUCCESS(hr) || !created) {
        Release(created);
        replay = nullptr;
        return false;
    }

    if (source) {
        source->AddRef();
    }
    s_replayDepthStencilStates.push_back({
        .source = source,
        .replay = created,
    });
    replay = created;
    REX::INFO(
        "ShadowMapDeferredLighting: cached write-suppressed depth/stencil "
        "replay state (source={}, depthWrites={}, stencilWrites={})",
        static_cast<const void*>(source),
        depthWrites,
        stencilWrites);
    return true;
}

bool EnsureAlwaysVisibleSampler(
    REX::W32::ID3D11Device* device,
    REX::W32::ID3D11SamplerState* source)
{
    if (!device || !source) {
        return false;
    }
    if (s_alwaysVisibleSampler &&
        s_alwaysVisibleSourceSampler == source) {
        return true;
    }

    REX::W32::D3D11_SAMPLER_DESC desc{};
    source->GetDesc(&desc);
    desc.comparisonFunc = REX::W32::D3D11_COMPARISON_ALWAYS;

    REX::W32::ID3D11SamplerState* sampler = nullptr;
    const HRESULT hr = device->CreateSamplerState(&desc, &sampler);
    if (!REX::W32::SUCCESS(hr) || !sampler) {
        REX::WARN(
            "ShadowMapDeferredLighting: always-visible comparison sampler "
            "creation failed 0x{:08X}",
            static_cast<unsigned>(hr));
        Release(sampler);
        return false;
    }

    Release(s_alwaysVisibleSampler);
    Release(s_alwaysVisibleSourceSampler);
    s_alwaysVisibleSampler = sampler;
    s_alwaysVisibleSourceSampler = source;
    s_alwaysVisibleSourceSampler->AddRef();
    return true;
}

struct ProjectionConstant
{
    std::array<std::uint32_t, 4> mode{};
    std::array<float, 4> viewport{};
};
static_assert(sizeof(ProjectionConstant) == 32);

struct ProjectionDomain
{
    std::uint32_t physicalWidth = 0;
    std::uint32_t physicalHeight = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    float dynamicWidthRatio = 1.0f;
    float dynamicHeightRatio = 1.0f;
    bool proxyAllocation = false;
    bool dynamicRatio = false;
    bool reducedViewport = false;
};

bool GetTexture2DExtent(
    REX::W32::ID3D11ShaderResourceView* view,
    std::uint32_t& width,
    std::uint32_t& height) noexcept
{
    if (!view) {
        return false;
    }

    REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    view->GetDesc(&viewDesc);
    if (viewDesc.viewDimension !=
        REX::W32::D3D11_SRV_DIMENSION_TEXTURE2D) {
        return false;
    }

    REX::W32::ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    REX::W32::ID3D11Texture2D* texture = nullptr;
    if (resource) {
        resource->QueryInterface(
            REX::W32::IID_ID3D11Texture2D,
            reinterpret_cast<void**>(&texture));
    }
    Release(resource);
    if (!texture) {
        return false;
    }

    REX::W32::D3D11_TEXTURE2D_DESC textureDesc{};
    texture->GetDesc(&textureDesc);
    Release(texture);

    const auto mip = (std::min)(
        viewDesc.texture2D.mostDetailedMip,
        textureDesc.mipLevels > 0 ? textureDesc.mipLevels - 1 : 0);
    width = (std::max)(textureDesc.width >> mip, 1u);
    height = (std::max)(textureDesc.height >> mip, 1u);
    return true;
}

bool ResolveProjectionDomain(
    REX::W32::ID3D11DeviceContext* context,
    const REX::W32::D3D11_VIEWPORT& viewport,
    ProjectionDomain& domain) noexcept
{
    REX::W32::ID3D11ShaderResourceView* depthView = nullptr;
    context->PSGetShaderResources(3, 1, &depthView);
    const bool havePhysicalExtent = GetTexture2DExtent(
        depthView, domain.physicalWidth, domain.physicalHeight);
    Release(depthView);

    const auto graphicsState = RE::BSGraphics::State::GetSingleton();
    domain.displayWidth = graphicsState.screenWidth;
    domain.displayHeight = graphicsState.screenHeight;
    if (domain.displayWidth == 0 || domain.displayHeight == 0) {
        domain.displayWidth = havePhysicalExtent ?
            domain.physicalWidth :
            (std::max)(
                static_cast<std::uint32_t>(std::lround(viewport.width)),
                1u);
        domain.displayHeight = havePhysicalExtent ?
            domain.physicalHeight :
            (std::max)(
                static_cast<std::uint32_t>(std::lround(viewport.height)),
                1u);
    }

    if (!havePhysicalExtent) {
        domain.physicalWidth = domain.displayWidth;
        domain.physicalHeight = domain.displayHeight;
    }

    const auto renderTargetManager =
        RE::BSGraphics::RenderTargetManager::GetSingleton();
    if (renderTargetManager.dynamicWidthRatio > 0.0f &&
        renderTargetManager.dynamicWidthRatio <= 1.0f) {
        domain.dynamicWidthRatio =
            renderTargetManager.dynamicWidthRatio;
    }
    if (renderTargetManager.dynamicHeightRatio > 0.0f &&
        renderTargetManager.dynamicHeightRatio <= 1.0f) {
        domain.dynamicHeightRatio =
            renderTargetManager.dynamicHeightRatio;
    }

    // Upscaling.cpp replaces Fallout's scene targets with physically reduced
    // proxy allocations. Its image-space hooks can temporarily restore a
    // full-display viewport and 1.0 dynamic-resolution ratios while those
    // proxies remain bound, so the physical t3 depth extent is authoritative
    // in that mode. Native Fallout DRS keeps display-sized allocations and
    // exposes the smaller projection domain through the manager ratios.
    domain.proxyAllocation =
        domain.physicalWidth < domain.displayWidth ||
        domain.physicalHeight < domain.displayHeight;
    domain.dynamicRatio =
        domain.dynamicWidthRatio < 0.999f ||
        domain.dynamicHeightRatio < 0.999f;
    domain.reducedViewport =
        viewport.width + 0.5f < static_cast<float>(domain.displayWidth) ||
        viewport.height + 0.5f < static_cast<float>(domain.displayHeight);

    if (domain.proxyAllocation) {
        domain.renderWidth = domain.physicalWidth;
        domain.renderHeight = domain.physicalHeight;
    } else if (domain.dynamicRatio) {
        domain.renderWidth = (std::max)(
            static_cast<std::uint32_t>(
                static_cast<float>(domain.displayWidth) *
                domain.dynamicWidthRatio),
            1u);
        domain.renderHeight = (std::max)(
            static_cast<std::uint32_t>(
                static_cast<float>(domain.displayHeight) *
                domain.dynamicHeightRatio),
            1u);
    } else if (domain.reducedViewport) {
        domain.renderWidth = (std::max)(
            static_cast<std::uint32_t>(std::lround(viewport.width)),
            1u);
        domain.renderHeight = (std::max)(
            static_cast<std::uint32_t>(std::lround(viewport.height)),
            1u);
    } else {
        domain.renderWidth = domain.physicalWidth;
        domain.renderHeight = domain.physicalHeight;
    }

    return domain.renderWidth > 0 && domain.renderHeight > 0;
}

bool UpdateProjectionConstant(
    REX::W32::ID3D11DeviceContext* context,
    Contract contract,
    REX::W32::D3D11_VIEWPORT& viewport,
    ProjectionDomain& domain) noexcept
{
    if (!context || !s_modeConstant) {
        return false;
    }

    UINT viewportCount = 1;
    context->RSGetViewports(&viewportCount, &viewport);
    if (viewportCount != 1 ||
        !(viewport.width > 0.0f) ||
        !(viewport.height > 0.0f)) {
        return false;
    }

    if (!ResolveProjectionDomain(context, viewport, domain)) {
        return false;
    }

    ProjectionConstant data{};
    data.mode[0] = static_cast<std::uint32_t>(contract);
    data.mode[1] = domain.renderWidth;
    data.mode[2] = domain.renderHeight;
    data.mode[3] =
        (domain.proxyAllocation ? 1u : 0u) |
        (domain.dynamicRatio ? 2u : 0u) |
        (domain.reducedViewport ? 4u : 0u);
    data.viewport = {
        viewport.topLeftX,
        viewport.topLeftY,
        1.0f / static_cast<float>(domain.renderWidth),
        1.0f / static_cast<float>(domain.renderHeight)
    };

    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!REX::W32::SUCCESS(context->Map(
            s_modeConstant,
            0,
            REX::W32::D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped))) {
        return false;
    }
    std::memcpy(mapped.data, &data, sizeof(data));
    context->Unmap(s_modeConstant, 0);
    return true;
}

bool CreateScratchTarget(
    REX::W32::ID3D11Device* device,
    const RE::BSGraphics::RenderTarget& source,
    ScratchTarget& target)
{
    if (!device || !source.texture || !source.rtView || !source.srView) {
        return false;
    }

    REX::W32::D3D11_TEXTURE2D_DESC sourceDesc{};
    source.texture->GetDesc(&sourceDesc);
    if (sourceDesc.sampleDesc.count != 1 ||
        sourceDesc.arraySize != 1) {
        return false;
    }
    if (target.texture && SameScratchDesc(target.sourceDesc, sourceDesc)) {
        return true;
    }

    target.Reset();
    REX::W32::D3D11_TEXTURE2D_DESC scratchDesc = sourceDesc;
    scratchDesc.usage = REX::W32::D3D11_USAGE_DEFAULT;
    scratchDesc.bindFlags =
        REX::W32::D3D11_BIND_RENDER_TARGET |
        REX::W32::D3D11_BIND_SHADER_RESOURCE;
    scratchDesc.cpuAccessFlags = 0;
    scratchDesc.miscFlags = 0;

    HRESULT hr =
        device->CreateTexture2D(&scratchDesc, nullptr, &target.texture);
    if (!REX::W32::SUCCESS(hr) || !target.texture) {
        target.Reset();
        return false;
    }

    REX::W32::D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    source.rtView->GetDesc(&rtvDesc);
    hr = device->CreateRenderTargetView(
        target.texture, &rtvDesc, &target.rtv);
    if (!REX::W32::SUCCESS(hr) || !target.rtv) {
        target.Reset();
        return false;
    }

    REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    source.srView->GetDesc(&srvDesc);
    hr = device->CreateShaderResourceView(
        target.texture, &srvDesc, &target.srv);
    if (!REX::W32::SUCCESS(hr) || !target.srv) {
        target.Reset();
        return false;
    }

    target.sourceDesc = sourceDesc;
    return true;
}

bool EnsureScratchTargets(REX::W32::ID3D11Device* device)
{
    if (!g_rendererData) {
        return false;
    }
    const auto diffuse =
        RT::idx(RT::Color::kDiffuseBuffer);
    const auto specular =
        RT::idx(RT::Color::kSpecularBuffer);
    const auto& diffuseTarget =
        g_rendererData->renderTargets[diffuse];
    const auto& specularTarget =
        g_rendererData->renderTargets[specular];
    if (!CreateScratchTarget(device, diffuseTarget, s_scratch[0]) ||
        !CreateScratchTarget(device, specularTarget, s_scratch[1]) ||
        !CreateScratchTarget(device, diffuseTarget, s_scratch[2]) ||
        !CreateScratchTarget(device, specularTarget, s_scratch[3])) {
        REX::WARN(
            "ShadowMapDeferredLighting: failed to mirror BGS deferred MRTs");
        return false;
    }
    return true;
}

struct LiveContract
{
    bool metadata = false;
    bool mrts = false;
    bool srvs = false;
    bool shadowViews = false;
    bool constants = false;
    bool sampler = false;
    bool replayDepthStencil = false;
    bool depthWrites = false;
    bool stencilWrites = false;
    std::uint32_t textureSlotMask = 0;
    std::uint32_t cb2Size = 0;
    std::uint32_t cb12Size = 0;
    int outputCount = 0;
    REX::W32::DXGI_FORMAT shadowFormat =
        REX::W32::DXGI_FORMAT_UNKNOWN;
    REX::W32::D3D11_SRV_DIMENSION shadowDimension =
        REX::W32::D3D11_SRV_DIMENSION_UNKNOWN;
    const void* shadowResource = nullptr;
    const void* diffuseRtv = nullptr;
    const void* specularRtv = nullptr;

    explicit operator bool() const noexcept
    {
        return metadata && mrts && srvs && shadowViews && constants &&
            sampler && replayDepthStencil;
    }
};

LiveContract ValidateLiveContract(
    REX::W32::ID3D11DeviceContext* context,
    const SavedState& saved,
    const ClassifiedContract& classified) noexcept
{
    LiveContract result{};
    result.textureSlotMask = classified.textureSlotMask;
    result.cb2Size = classified.cbSizes[2];
    result.cb12Size = classified.cbSizes[12];
    result.outputCount = classified.outputCount;
    const auto requiredCb2Size =
        classified.IsDirectional() ? 28u * 16u : 23u * 16u;
    result.metadata =
        classified.outputCount == 2 &&
        (classified.textureSlotMask & (1u << 2)) != 0 &&
        (classified.textureSlotMask & (1u << 3)) != 0 &&
        (classified.textureSlotMask & (1u << 5)) != 0 &&
        classified.cbSizes[2] >= requiredCb2Size &&
        classified.cbSizes[12] >= 28u * 16u;
    if (!context || !g_rendererData || !saved.captured ||
        !saved.pixelShader) {
        return result;
    }

    const auto diffuse =
        RT::idx(RT::Color::kDiffuseBuffer);
    const auto specular =
        RT::idx(RT::Color::kSpecularBuffer);
    result.mrts =
        saved.rtvs[0] == g_rendererData->renderTargets[diffuse].rtView &&
        saved.rtvs[1] == g_rendererData->renderTargets[specular].rtView;
    result.diffuseRtv = saved.rtvs[0];
    result.specularRtv = saved.rtvs[1];

    std::array<REX::W32::ID3D11ShaderResourceView*, 3> srvs{};
    context->PSGetShaderResources(2, 2, srvs.data());
    context->PSGetShaderResources(5, 1, &srvs[2]);
    std::array<REX::W32::ID3D11Buffer*, 2> constants{};
    context->PSGetConstantBuffers(2, 1, &constants[0]);
    context->PSGetConstantBuffers(12, 1, &constants[1]);
    REX::W32::ID3D11SamplerState* comparisonSampler = nullptr;
    context->PSGetSamplers(5, 1, &comparisonSampler);

    result.srvs = srvs[0] && srvs[1] && srvs[2];
    result.constants = constants[0] && constants[1];
    result.sampler = comparisonSampler != nullptr;

    if (srvs[2]) {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC shadowDesc{};
        srvs[2]->GetDesc(&shadowDesc);

        REX::W32::ID3D11Resource* shadowResource = nullptr;
        srvs[2]->GetResource(&shadowResource);
        result.shadowViews =
            shadowResource &&
            shadowDesc.viewDimension ==
                REX::W32::D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        result.shadowFormat = shadowDesc.format;
        result.shadowDimension = shadowDesc.viewDimension;
        result.shadowResource = shadowResource;
        Release(shadowResource);
    }

    for (auto*& srv : srvs) {
        Release(srv);
    }
    for (auto*& constant : constants) {
        Release(constant);
    }
    Release(comparisonSampler);
    return result;
}

}  // namespace

void ObservePass(BSRenderPassLayout* pass) noexcept
{
    if (!SHADERENGINE_EFFECTS_ON ||
        !SHADOW_UPGRADE_ON ||
        !ShadowUpgrade::IsInDeferredLighting() ||
        !pass ||
        !pass->shader ||
        pass->shader->shaderType != 4) {
        return;
    }

    const auto techniqueID = pass->techniqueID;
    const auto pixelShaderID = GetPixelShaderID(techniqueID);
    const auto contract = ClassifyPixelShaderID(pixelShaderID);
    if (!contract) {
        return;
    }

    auto* originalShader =
        g_currentOriginalPixelShader.load(std::memory_order_acquire);
    if (!originalShader) {
        return;
    }

    std::unique_lock selectionLock(s_selectionMutex);
    s_observedSelections.insert_or_assign(
        originalShader,
        ObservedSelection{
            .contract = *contract,
            .techniqueID = techniqueID,
            .pixelShaderID = pixelShaderID,
        });
}

bool TryRenderLiveDraw(
    REX::W32::ID3D11DeviceContext* context,
    LiveDraw_t draw,
    const void* payload) noexcept
{
    if (!SHADERENGINE_EFFECTS_ON ||
        !SHADOW_UPGRADE_ON ||
        !TracedDeferredLightingEnabled() ||
        g_customPassRendering ||
        !context ||
        !draw ||
        !g_rendererData ||
        !g_rendererData->device) {
        return false;
    }

    auto* device = g_rendererData->device;
    if (!ScratchSlotsAreAvailable()) {
        if (!s_slotConflictLogged) {
            REX::WARN(
                "ShadowMapDeferredLighting: scratch SRV slots t26-t29 "
                "conflict with configured ShaderEngine resource slots; "
                "vanilla draws retained");
            s_slotConflictLogged = true;
        }
        return false;
    }

    SavedState saved(context);
    const auto contract = Classify(
        g_currentOriginalPixelShader.load(std::memory_order_acquire),
        saved.pixelShader);
    if (!contract) {
        return false;
    }

    auto liveContract =
        ValidateLiveContract(context, saved, *contract);
    REX::W32::ID3D11DepthStencilState* replayDepthStencil = nullptr;
    liveContract.replayDepthStencil =
        EnsureWriteSuppressedDepthStencilState(
            device,
            saved.dsv,
            saved.depthStencil,
            replayDepthStencil,
            liveContract.depthWrites,
            liveContract.stencilWrites);
    if (!liveContract) {
        std::lock_guard diagnosticLock(s_diagnosticMutex);
        if (s_rejectedSelections.insert(SelectionKey(*contract)).second) {
            REX::WARN(
                "ShadowMapDeferredLighting: techniqueID=0x{:08X} "
                "pixelShaderID=0x{:08X} contract={} rejected live D3D11 "
                "contract (metadata={}, MRTs={}, SRVs={}, shadowView={}, "
                "CBs={}, sampler={}, replayDS={}, depthWrites={}, "
                "stencilWrites={}, textureMask=0x{:08X}, cb2={}, cb12={}, "
                "outputs={}, shadowFormat={}, shadowDimension={}, "
                "shadowResource={}, rtv0={}, rtv1={}); "
                "vanilla draw retained",
                contract->techniqueID,
                contract->pixelShaderID,
                static_cast<std::uint32_t>(contract->contract),
                liveContract.metadata,
                liveContract.mrts,
                liveContract.srvs,
                liveContract.shadowViews,
                liveContract.constants,
                liveContract.sampler,
                liveContract.replayDepthStencil,
                liveContract.depthWrites,
                liveContract.stencilWrites,
                liveContract.textureSlotMask,
                liveContract.cb2Size,
                liveContract.cb12Size,
                liveContract.outputCount,
                static_cast<std::uint32_t>(liveContract.shadowFormat),
                static_cast<std::uint32_t>(liveContract.shadowDimension),
                liveContract.shadowResource,
                liveContract.diffuseRtv,
                liveContract.specularRtv);
        }
        return false;
    }

    const auto shaderKind =
        contract->IsDirectional() ?
            ShaderKind::kDirectional :
            ShaderKind::kLocal;
    const bool needsUnshadowedDirectional =
        contract->contract == Contract::kDirectionalLighting;
    if (!EnsureShader(device, shaderKind) ||
        !EnsureFixedStates(device) ||
        !EnsureScratchTargets(device) ||
        (needsUnshadowedDirectional &&
            !EnsureAlwaysVisibleSampler(device, saved.shadowSampler))) {
        return false;
    }

    REX::W32::D3D11_VIEWPORT viewport{};
    ProjectionDomain projectionDomain{};
    if (!UpdateProjectionConstant(
            context,
            contract->contract,
            viewport,
            projectionDomain)) {
        return false;
    }

    ScopedCustomDraw customScope;
    saved.MarkModified();

    // Isolate the exact output of this one BGS light. Opaque scratch blending
    // avoids coupling the upgraded visibility to previously accumulated lights.
    REX::W32::ID3D11RenderTargetView* scratchRtvs[2]{
        s_scratch[kShadowedScratchIndex].rtv,
        s_scratch[kShadowedScratchIndex + 1].rtv
    };
    context->OMSetDepthStencilState(
        replayDepthStencil, saved.stencilRef);
    context->OMSetRenderTargets(2, scratchRtvs, saved.dsv);
    context->OMSetBlendState(s_opaqueBlend, nullptr, 0xFFFFFFFFu);
    draw(context, payload);

    // pixelDeferredLightOG combines directional direct light with ambient/SH
    // terms. Replay it once with an always-visible comparison sampler so the
    // composite shader can solve S = A + D*V and U = A + D, then alter only D.
    // The pure visibility contract and local-light contracts do not need this
    // additional draw.
    if (needsUnshadowedDirectional) {
        REX::W32::ID3D11RenderTargetView* unshadowedRtvs[2]{
            s_scratch[0].rtv,
            s_scratch[1].rtv
        };
        context->OMSetRenderTargets(2, unshadowedRtvs, saved.dsv);
        context->PSSetSamplers(5, 1, &s_alwaysVisibleSampler);
        draw(context, payload);
        context->PSSetSamplers(5, 1, &saved.shadowSampler);
    }

    // The capture draws used the write-suppressed clone, so the final
    // replacement sees the same depth/stencil contents as the original draw.
    // Restore the live state here so Fallout's intended depth/stencil updates
    // occur exactly once. Every fragment that can read scratch was overwritten
    // by the opaque first draw, so no full-screen scratch clear is required.
    context->OMSetDepthStencilState(saved.depthStencil, saved.stencilRef);
    context->OMSetRenderTargets(
        static_cast<UINT>(saved.rtvs.size()),
        saved.rtvs.data(),
        saved.dsv);
    context->OMSetBlendState(
        saved.blend, saved.blendFactor, saved.sampleMask);
    context->PSSetShader(
        s_filterShaders[static_cast<std::size_t>(shaderKind)], nullptr, 0);

    REX::W32::ID3D11ShaderResourceView*
        scratchSrvs[kScratchTargetCount]{
            s_scratch[0].srv,
            s_scratch[1].srv,
            s_scratch[2].srv,
            s_scratch[3].srv
    };
    context->PSSetShaderResources(
        kScratchSrvSlot, kScratchTargetCount, scratchSrvs);
    context->PSSetConstantBuffers(
        kModeConstantSlot, 1, &s_modeConstant);

    if (g_modularFloatsSRV) {
        context->PSSetShaderResources(
            MODULAR_FLOATS_SLOT, 1, &g_modularFloatsSRV);
    }
    if (g_modularIntsSRV) {
        context->PSSetShaderResources(
            MODULAR_INTS_SLOT, 1, &g_modularIntsSRV);
    }
    if (g_modularBoolsSRV) {
        context->PSSetShaderResources(
            MODULAR_BOOLS_SLOT, 1, &g_modularBoolsSRV);
    }

    draw(context, payload);

    {
        std::lock_guard diagnosticLock(s_diagnosticMutex);
        if (s_acceptedSelections.insert(SelectionKey(*contract)).second) {
            REX::INFO(
                "ShadowMapDeferredLighting: techniqueID=0x{:08X} "
                "pixelShaderID=0x{:08X} contract={} {} draw isolated and "
                "recomposited through live t5 "
                "(shadowFormat={}, textureMask=0x{:08X})",
                contract->techniqueID,
                contract->pixelShaderID,
                static_cast<std::uint32_t>(contract->contract),
                contract->IsDirectional() ? "directional" : "local-shadow",
                static_cast<std::uint32_t>(liveContract.shadowFormat),
                liveContract.textureSlotMask);
            REX::INFO(
                "ShadowMapDeferredLighting: projection domain viewport="
                "({}, {}, {}x{}), render={}x{}, physical={}x{}, "
                "display={}x{}, ratio=({}, {}), flags=0x{:X}",
                viewport.topLeftX,
                viewport.topLeftY,
                viewport.width,
                viewport.height,
                projectionDomain.renderWidth,
                projectionDomain.renderHeight,
                projectionDomain.physicalWidth,
                projectionDomain.physicalHeight,
                projectionDomain.displayWidth,
                projectionDomain.displayHeight,
                projectionDomain.dynamicWidthRatio,
                projectionDomain.dynamicHeightRatio,
                (projectionDomain.proxyAllocation ? 1u : 0u) |
                    (projectionDomain.dynamicRatio ? 2u : 0u) |
                    (projectionDomain.reducedViewport ? 4u : 0u));
        }
    }
    // Restore while custom rendering suppression is still active. Otherwise
    // the PSSetShader hook would mistake the saved replacement object for a
    // new engine/original shader and corrupt the current ShaderDB identity.
    saved.Restore();
    return true;
}

void InvalidateShader() noexcept
{
    for (auto*& shader : s_filterShaders) {
        Release(shader);
    }
    s_compileTried.fill(false);
    s_compileFailed.fill(false);
    {
        std::lock_guard diagnosticLock(s_diagnosticMutex);
        s_acceptedSelections.clear();
        s_rejectedSelections.clear();
    }
    s_slotConflictLogged = false;
}

void Shutdown() noexcept
{
    InvalidateShader();
    {
        std::unique_lock selectionLock(s_selectionMutex);
        s_observedSelections.clear();
    }
    for (auto& target : s_scratch) {
        target.Reset();
    }
    Release(s_opaqueBlend);
    Release(s_modeConstant);
    Release(s_alwaysVisibleSampler);
    Release(s_alwaysVisibleSourceSampler);
    {
        std::lock_guard lock(s_depthStencilMutex);
        for (auto& state : s_replayDepthStencilStates) {
            Release(state.replay);
            Release(state.source);
        }
        s_replayDepthStencilStates.clear();
    }
}

}  // namespace ShadowMapDeferredLighting
