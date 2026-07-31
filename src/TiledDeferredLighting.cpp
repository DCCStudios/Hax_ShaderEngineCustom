#include <TiledDeferredLighting.h>

#include <Global.h>
#include <Plugin.h>
#include <RenderTargets.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace TiledDeferredLighting {
namespace {

constexpr UINT kTiledDiffuseSlot = 11;
constexpr UINT kTiledSpecularSlot = 12;
constexpr UINT kCapturedLightSlot = 25;
constexpr UINT kModeConstantSlot = 13;
constexpr std::size_t kMaximumCapturedLights = 625;
constexpr std::size_t kMaximumTracedLights = 16;
constexpr const char* kShaderFile = "TiledDeferredLighting.hlsl";
constexpr std::uint32_t kTiledLighting = 1u << 16;

std::uint32_t GetTiledPixelShaderID(
    std::uint32_t techniqueID) noexcept
{
    // Tiled branch of BSDFCompositeShaderMacros::GetPixelShaderID
    // @ 0x1428DEB60 (FO4 1.10.163). DrawWorld::QTiledLighting chooses this
    // branch and forces the TILELIGHT macro into the FXP lookup key.
    return (techniqueID & 0xFFFFFBE9u) | kTiledLighting;
}

template <class T>
void Release(T*& value) noexcept
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

struct CapturedLight
{
    std::array<float, 4> positionAndRadius{};
    std::array<float, 4> colorAndFlags{};
    std::array<float, 4> directionAndId{};
};

static_assert(sizeof(CapturedLight) == 48);
static_assert(offsetof(CapturedLight, positionAndRadius) == 0);
static_assert(offsetof(CapturedLight, colorAndFlags) == 16);
static_assert(offsetof(CapturedLight, directionAndId) == 32);

struct ScoredLight
{
    CapturedLight gpu{};
    float score = 0.0f;
};

struct FrameState
{
    std::vector<ScoredLight> captured;
    std::array<CapturedLight, kMaximumTracedLights> selected{};
    std::uint32_t selectedCount = 0;
    bool collecting = false;
    bool readyForComposite = false;
};

thread_local FrameState s_frame{};

struct ScratchTarget
{
    REX::W32::ID3D11Texture2D* texture = nullptr;
    REX::W32::ID3D11RenderTargetView* rtv = nullptr;
    REX::W32::ID3D11ShaderResourceView* srv = nullptr;
    REX::W32::D3D11_TEXTURE2D_DESC textureDesc{};
    REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};

    void Reset() noexcept
    {
        Release(srv);
        Release(rtv);
        Release(texture);
        textureDesc = {};
        srvDesc = {};
    }
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
    std::array<REX::W32::ID3D11ShaderResourceView*, 2> tiledInputs{};
    REX::W32::ID3D11ShaderResourceView* capturedLights = nullptr;
    REX::W32::ID3D11ShaderResourceView* customInput = nullptr;
    std::array<REX::W32::ID3D11ShaderResourceView*, 3> modularInputs{};
    REX::W32::ID3D11Buffer* modeConstant = nullptr;
    bool captured = false;
    bool modified = false;

    explicit SavedState(
        REX::W32::ID3D11DeviceContext* a_context) noexcept :
        context(a_context)
    {
        if (!context) {
            return;
        }
        context->OMGetRenderTargets(
            static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
        context->OMGetBlendState(
            &blend, blendFactor, &sampleMask);
        context->OMGetDepthStencilState(
            &depthStencil, &stencilRef);
        context->PSGetShader(
            &pixelShader, nullptr, nullptr);
        context->PSGetShaderResources(
            kTiledDiffuseSlot,
            static_cast<UINT>(tiledInputs.size()),
            tiledInputs.data());
        context->PSGetShaderResources(
            kCapturedLightSlot, 1, &capturedLights);
        context->PSGetShaderResources(
            CUSTOMBUFFER_SLOT, 1, &customInput);
        context->PSGetShaderResources(
            MODULAR_FLOATS_SLOT, 1, &modularInputs[0]);
        context->PSGetShaderResources(
            MODULAR_INTS_SLOT, 1, &modularInputs[1]);
        context->PSGetShaderResources(
            MODULAR_BOOLS_SLOT, 1, &modularInputs[2]);
        context->PSGetConstantBuffers(
            kModeConstantSlot, 1, &modeConstant);
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
        for (auto*& input : tiledInputs) {
            Release(input);
        }
        Release(capturedLights);
        Release(customInput);
        for (auto*& input : modularInputs) {
            Release(input);
        }
        Release(modeConstant);
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
            kTiledDiffuseSlot,
            static_cast<UINT>(tiledInputs.size()),
            tiledInputs.data());
        context->PSSetShaderResources(
            kCapturedLightSlot, 1, &capturedLights);
        context->PSSetShaderResources(
            CUSTOMBUFFER_SLOT, 1, &customInput);
        context->PSSetShaderResources(
            MODULAR_FLOATS_SLOT, 1, &modularInputs[0]);
        context->PSSetShaderResources(
            MODULAR_INTS_SLOT, 1, &modularInputs[1]);
        context->PSSetShaderResources(
            MODULAR_BOOLS_SLOT, 1, &modularInputs[2]);
        context->PSSetConstantBuffers(
            kModeConstantSlot, 1, &modeConstant);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->OMSetDepthStencilState(
            depthStencil, stencilRef);
        context->OMSetBlendState(
            blend, blendFactor, sampleMask);
        context->OMSetRenderTargets(
            static_cast<UINT>(rtvs.size()),
            rtvs.data(),
            dsv);
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

struct CompositeContract
{
    std::uint32_t techniqueID = 0;
    std::uint32_t pixelShaderID = 0;
    std::uint32_t textureSlotMask = 0;
    std::uint32_t cb2Size = 0;
    int outputCount = 0;
};

struct ObservedComposite
{
    std::uint32_t techniqueID = 0;
    std::uint32_t pixelShaderID = 0;
};

struct LiveContract
{
    bool metadata = false;
    bool mainTarget = false;
    bool singleTarget = false;
    bool tiledInputs = false;
    bool depthInput = false;
    bool constants = false;
    REX::W32::D3D11_SRV_DIMENSION diffuseDimension =
        REX::W32::D3D11_SRV_DIMENSION_UNKNOWN;
    REX::W32::D3D11_SRV_DIMENSION specularDimension =
        REX::W32::D3D11_SRV_DIMENSION_UNKNOWN;
    REX::W32::D3D11_SRV_DIMENSION depthDimension =
        REX::W32::D3D11_SRV_DIMENSION_UNKNOWN;

    explicit operator bool() const noexcept
    {
        return metadata &&
            mainTarget &&
            singleTarget &&
            tiledInputs &&
            depthInput &&
            constants;
    }
};

std::array<ScratchTarget, 2> s_scratch{};
REX::W32::ID3D11PixelShader* s_processorShader = nullptr;
REX::W32::ID3D11BlendState* s_opaqueBlend = nullptr;
REX::W32::ID3D11DepthStencilState* s_depthDisabled = nullptr;
REX::W32::ID3D11Buffer* s_lightBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* s_lightSrv = nullptr;
REX::W32::ID3D11Buffer* s_modeConstant = nullptr;
bool s_compileTried = false;
bool s_compileFailed = false;
bool s_slotConflictLogged = false;
bool s_resourceFailureLogged = false;
std::unordered_map<REX::W32::ID3D11PixelShader*, ObservedComposite>
    s_observedComposites;
std::shared_mutex s_compositeMutex;
std::unordered_set<std::uint64_t> s_acceptedSelections;
std::unordered_set<std::uint64_t> s_rejectedSelections;
std::mutex s_diagnosticMutex;

std::uint64_t SelectionKey(
    const CompositeContract& contract) noexcept
{
    return
        (static_cast<std::uint64_t>(contract.pixelShaderID) << 32) |
        contract.techniqueID;
}

bool SameTextureDesc(
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

bool SameSrvDesc(
    const REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC& a,
    const REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC& b) noexcept
{
    return a.format == b.format &&
        a.viewDimension == b.viewDimension &&
        a.texture2D.mostDetailedMip ==
            b.texture2D.mostDetailedMip &&
        a.texture2D.mipLevels == b.texture2D.mipLevels;
}

bool ProcessorSlotsAreAvailable() noexcept
{
    constexpr std::array<UINT, 3> processorSlots{
        kTiledDiffuseSlot,
        kTiledSpecularSlot,
        kCapturedLightSlot
    };
    const std::array<UINT, 5> injectedSlots{
        CUSTOMBUFFER_SLOT,
        DRAWTAG_SLOT,
        MODULAR_FLOATS_SLOT,
        MODULAR_INTS_SLOT,
        MODULAR_BOOLS_SLOT
    };
    for (const auto processorSlot : processorSlots) {
        if (std::find(
                injectedSlots.begin(),
                injectedSlots.end(),
                processorSlot) != injectedSlots.end()) {
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

bool EnsureShader(REX::W32::ID3D11Device* device)
{
    if (s_processorShader) {
        return true;
    }
    if (s_compileFailed || s_compileTried || !device) {
        return false;
    }
    s_compileTried = true;

    const auto path = g_commonShaderHeaderPath / kShaderFile;
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        REX::WARN(
            "TiledDeferredLighting: shader file not found: {}",
            path.string());
        s_compileFailed = true;
        return false;
    }
    const std::string body{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };
    const std::string source = AssembleShaderSource(body);

    constexpr std::uint32_t kCompileFlags =
        D3DCOMPILE_OPTIMIZATION_LEVEL3;
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
            kShaderFile,
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
                    "TiledDeferredLighting: compile failed: {}",
                    static_cast<const char*>(
                        errors->GetBufferPointer()));
            } else {
                REX::WARN(
                    "TiledDeferredLighting: compile failed 0x{:08X}",
                    static_cast<unsigned>(hr));
            }
            Release(errors);
            Release(blob);
            s_compileFailed = true;
            return false;
        }
        Release(errors);
        ShaderCache::Store(cacheKey, blob);
    }

    const bool previousCreationFlag =
        g_isCreatingReplacementShader;
    g_isCreatingReplacementShader = true;
    const HRESULT hr = device->CreatePixelShader(
        blob->GetBufferPointer(),
        blob->GetBufferSize(),
        nullptr,
        &s_processorShader);
    g_isCreatingReplacementShader = previousCreationFlag;
    const auto byteCount = blob->GetBufferSize();
    Release(blob);

    if (!REX::W32::SUCCESS(hr) || !s_processorShader) {
        REX::WARN(
            "TiledDeferredLighting: CreatePixelShader failed "
            "0x{:08X}",
            static_cast<unsigned>(hr));
        s_compileFailed = true;
        return false;
    }

    REX::INFO(
        "TiledDeferredLighting: compiled tiled-accumulator "
        "visibility processor ({} bytes)",
        byteCount);
    return true;
}

bool EnsureFixedResources(REX::W32::ID3D11Device* device)
{
    if (!device) {
        return false;
    }

    HRESULT hr = S_OK;
    if (!s_opaqueBlend) {
        REX::W32::D3D11_BLEND_DESC desc{};
        desc.renderTarget[0].renderTargetWriteMask =
            REX::W32::D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(
            &desc, &s_opaqueBlend);
        if (!REX::W32::SUCCESS(hr)) {
            return false;
        }
    }
    if (!s_depthDisabled) {
        REX::W32::D3D11_DEPTH_STENCIL_DESC desc{};
        desc.depthEnable = false;
        desc.depthWriteMask =
            REX::W32::D3D11_DEPTH_WRITE_MASK_ZERO;
        desc.depthFunc =
            REX::W32::D3D11_COMPARISON_ALWAYS;
        hr = device->CreateDepthStencilState(
            &desc, &s_depthDisabled);
        if (!REX::W32::SUCCESS(hr)) {
            return false;
        }
    }
    if (!s_lightBuffer) {
        REX::W32::D3D11_BUFFER_DESC desc{};
        desc.byteWidth =
            static_cast<UINT>(
                sizeof(CapturedLight) *
                kMaximumTracedLights);
        desc.usage = REX::W32::D3D11_USAGE_DYNAMIC;
        desc.bindFlags =
            REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.cpuAccessFlags =
            REX::W32::D3D11_CPU_ACCESS_WRITE;
        desc.miscFlags =
            REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.structureByteStride =
            static_cast<UINT>(sizeof(CapturedLight));
        hr = device->CreateBuffer(
            &desc, nullptr, &s_lightBuffer);
        if (!REX::W32::SUCCESS(hr) || !s_lightBuffer) {
            return false;
        }

        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.format = REX::W32::DXGI_FORMAT_UNKNOWN;
        srvDesc.viewDimension =
            REX::W32::D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.buffer.firstElement = 0;
        srvDesc.buffer.numElements =
            static_cast<UINT>(kMaximumTracedLights);
        hr = device->CreateShaderResourceView(
            s_lightBuffer, &srvDesc, &s_lightSrv);
        if (!REX::W32::SUCCESS(hr) || !s_lightSrv) {
            Release(s_lightSrv);
            Release(s_lightBuffer);
            return false;
        }
    }
    if (!s_modeConstant) {
        REX::W32::D3D11_BUFFER_DESC desc{};
        desc.byteWidth = 16;
        desc.usage = REX::W32::D3D11_USAGE_DYNAMIC;
        desc.bindFlags =
            REX::W32::D3D11_BIND_CONSTANT_BUFFER;
        desc.cpuAccessFlags =
            REX::W32::D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(
            &desc, nullptr, &s_modeConstant);
        if (!REX::W32::SUCCESS(hr) || !s_modeConstant) {
            Release(s_modeConstant);
            return false;
        }
    }
    return s_opaqueBlend &&
        s_depthDisabled &&
        s_lightBuffer &&
        s_lightSrv &&
        s_modeConstant;
}

bool CreateScratchTarget(
    REX::W32::ID3D11Device* device,
    REX::W32::ID3D11ShaderResourceView* sourceView,
    ScratchTarget& target)
{
    if (!device || !sourceView) {
        return false;
    }

    REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC sourceSrvDesc{};
    sourceView->GetDesc(&sourceSrvDesc);
    if (sourceSrvDesc.viewDimension !=
        REX::W32::D3D11_SRV_DIMENSION_TEXTURE2D) {
        return false;
    }

    REX::W32::ID3D11Resource* sourceResource = nullptr;
    sourceView->GetResource(&sourceResource);
    REX::W32::ID3D11Texture2D* sourceTexture = nullptr;
    if (sourceResource) {
        sourceResource->QueryInterface(
            REX::W32::IID_ID3D11Texture2D,
            reinterpret_cast<void**>(&sourceTexture));
    }
    Release(sourceResource);
    if (!sourceTexture) {
        return false;
    }

    REX::W32::D3D11_TEXTURE2D_DESC sourceDesc{};
    sourceTexture->GetDesc(&sourceDesc);
    Release(sourceTexture);
    if (sourceDesc.sampleDesc.count != 1 ||
        sourceDesc.arraySize != 1 ||
        sourceDesc.mipLevels != 1) {
        return false;
    }

    if (target.texture &&
        SameTextureDesc(target.textureDesc, sourceDesc) &&
        SameSrvDesc(target.srvDesc, sourceSrvDesc)) {
        return true;
    }

    target.Reset();
    auto scratchDesc = sourceDesc;
    scratchDesc.usage = REX::W32::D3D11_USAGE_DEFAULT;
    scratchDesc.bindFlags =
        REX::W32::D3D11_BIND_RENDER_TARGET |
        REX::W32::D3D11_BIND_SHADER_RESOURCE;
    scratchDesc.cpuAccessFlags = 0;
    scratchDesc.miscFlags = 0;

    HRESULT hr = device->CreateTexture2D(
        &scratchDesc, nullptr, &target.texture);
    if (!REX::W32::SUCCESS(hr) || !target.texture) {
        target.Reset();
        return false;
    }

    REX::W32::D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.format = sourceSrvDesc.format;
    rtvDesc.viewDimension =
        REX::W32::D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.texture2D.mipSlice =
        sourceSrvDesc.texture2D.mostDetailedMip;
    hr = device->CreateRenderTargetView(
        target.texture, &rtvDesc, &target.rtv);
    if (!REX::W32::SUCCESS(hr) || !target.rtv) {
        target.Reset();
        return false;
    }

    hr = device->CreateShaderResourceView(
        target.texture, &sourceSrvDesc, &target.srv);
    if (!REX::W32::SUCCESS(hr) || !target.srv) {
        target.Reset();
        return false;
    }

    target.textureDesc = sourceDesc;
    target.srvDesc = sourceSrvDesc;
    return true;
}

bool EnsureScratchTargets(
    REX::W32::ID3D11Device* device,
    const SavedState& saved)
{
    return
        CreateScratchTarget(
            device,
            saved.tiledInputs[0],
            s_scratch[0]) &&
        CreateScratchTarget(
            device,
            saved.tiledInputs[1],
            s_scratch[1]);
}

bool UpdateGpuInputs(
    REX::W32::ID3D11DeviceContext* context)
{
    if (!context ||
        !s_lightBuffer ||
        !s_modeConstant ||
        s_frame.selectedCount == 0) {
        return false;
    }

    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
    if (!REX::W32::SUCCESS(context->Map(
            s_lightBuffer,
            0,
            REX::W32::D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped))) {
        return false;
    }
    std::memcpy(
        mapped.data,
        s_frame.selected.data(),
        sizeof(s_frame.selected));
    context->Unmap(s_lightBuffer, 0);

    mapped = {};
    if (!REX::W32::SUCCESS(context->Map(
            s_modeConstant,
            0,
            REX::W32::D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped))) {
        return false;
    }
    const std::array<std::uint32_t, 4> mode{
        s_frame.selectedCount,
        0,
        0,
        0
    };
    std::memcpy(mapped.data, mode.data(), sizeof(mode));
    context->Unmap(s_modeConstant, 0);
    return true;
}

std::optional<CompositeContract> Classify(
    REX::W32::ID3D11PixelShader* originalShader,
    REX::W32::ID3D11PixelShader* activeShader) noexcept
{
    if (!originalShader || !activeShader) {
        return std::nullopt;
    }

    CompositeContract contract{};
    {
        std::shared_lock compositeLock(s_compositeMutex);
        const auto compositeIt =
            s_observedComposites.find(originalShader);
        if (compositeIt == s_observedComposites.end()) {
            return std::nullopt;
        }
        contract.techniqueID =
            compositeIt->second.techniqueID;
        contract.pixelShaderID =
            compositeIt->second.pixelShaderID;
    }
    if ((contract.pixelShaderID & kTiledLighting) == 0) {
        return std::nullopt;
    }

    REX::W32::ID3D11PixelShader* replacementShader = nullptr;
    {
        std::shared_lock lock(g_ShaderDB.mutex);
        const auto it = g_ShaderDB.entries.find(originalShader);
        if (it == g_ShaderDB.entries.end() ||
            it->second.type != ShaderType::Pixel) {
            return std::nullopt;
        }
        contract.textureSlotMask =
            it->second.textureSlotMask;
        contract.cb2Size =
            it->second.expectedCBSizes[2];
        contract.outputCount =
            it->second.outputCount;
        replacementShader =
            it->second.replacementPixelShader.load(
                std::memory_order_acquire);
    }

    if (activeShader != originalShader &&
        activeShader != replacementShader) {
        return std::nullopt;
    }

    constexpr std::uint32_t kRequiredTextures =
        (1u << kTiledDiffuseSlot) |
        (1u << kTiledSpecularSlot);
    if ((contract.textureSlotMask & kRequiredTextures) !=
            kRequiredTextures ||
        contract.outputCount != 1 ||
        contract.cb2Size < 16) {
        return std::nullopt;
    }
    return contract;
}

LiveContract ValidateLiveContract(
    REX::W32::ID3D11DeviceContext* context,
    const SavedState& saved,
    const CompositeContract& contract) noexcept
{
    LiveContract result{};
    constexpr std::uint32_t kRequiredTextures =
        (1u << kTiledDiffuseSlot) |
        (1u << kTiledSpecularSlot);
    result.metadata =
        (contract.textureSlotMask & kRequiredTextures) ==
            kRequiredTextures &&
        contract.outputCount == 1 &&
        contract.cb2Size >= 16;
    if (!context ||
        !g_rendererData ||
        !saved.captured ||
        !saved.pixelShader) {
        return result;
    }

    const auto main =
        RT::idx(RT::Color::kMain);
    const auto mainPreAlpha =
        RT::idx(RT::Color::kMainPreAlpha);
    result.mainTarget =
        saved.rtvs[0] ==
            g_rendererData->renderTargets[main].rtView ||
        saved.rtvs[0] ==
            g_rendererData->renderTargets[mainPreAlpha].rtView;
    result.singleTarget =
        saved.rtvs[0] && !saved.rtvs[1];

    REX::W32::ID3D11ShaderResourceView* depthInput = nullptr;
    context->PSGetShaderResources(7, 1, &depthInput);
    REX::W32::ID3D11Buffer* cb2 = nullptr;
    context->PSGetConstantBuffers(2, 1, &cb2);
    result.constants = cb2 != nullptr;

    if (saved.tiledInputs[0]) {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        saved.tiledInputs[0]->GetDesc(&desc);
        result.diffuseDimension = desc.viewDimension;
    }
    if (saved.tiledInputs[1]) {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        saved.tiledInputs[1]->GetDesc(&desc);
        result.specularDimension = desc.viewDimension;
    }
    if (depthInput) {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        depthInput->GetDesc(&desc);
        result.depthDimension = desc.viewDimension;
    }

    result.tiledInputs =
        saved.tiledInputs[0] &&
        saved.tiledInputs[1] &&
        result.diffuseDimension ==
            REX::W32::D3D11_SRV_DIMENSION_TEXTURE2D &&
        result.specularDimension ==
            REX::W32::D3D11_SRV_DIMENSION_TEXTURE2D;
    result.depthInput =
        depthInput &&
        result.depthDimension ==
            REX::W32::D3D11_SRV_DIMENSION_TEXTURE2D;

    Release(depthInput);
    Release(cb2);
    return result;
}

}  // namespace

void RecordLight(
    std::uint32_t id,
    const RE::NiPoint3* position,
    float radius,
    const RE::NiColor* color,
    const RE::NiPoint3* direction,
    bool flagA,
    bool flagB,
    bool flagC,
    bool flagD) noexcept
{
    if (!SHADERENGINE_EFFECTS_ON ||
        !SHADOW_UPGRADE_ON ||
        !position ||
        !color ||
        !std::isfinite(radius) ||
        radius <= 0.0f) {
        return;
    }

    if (!s_frame.collecting) {
        s_frame.captured.clear();
        s_frame.selected = {};
        s_frame.selectedCount = 0;
        s_frame.readyForComposite = false;
        s_frame.collecting = true;
        if (s_frame.captured.capacity() <
            kMaximumCapturedLights) {
            s_frame.captured.reserve(
                kMaximumCapturedLights);
        }
    }
    if (s_frame.captured.size() >=
        kMaximumCapturedLights) {
        return;
    }

    const std::uint32_t flags =
        (flagA ? 1u : 0u) |
        (flagB ? 2u : 0u) |
        (flagC ? 4u : 0u) |
        (flagD ? 8u : 0u);
    ScoredLight light{};
    light.gpu.positionAndRadius = {
        position->x,
        position->y,
        position->z,
        radius
    };
    light.gpu.colorAndFlags = {
        color->r,
        color->g,
        color->b,
        static_cast<float>(flags)
    };
    light.gpu.directionAndId = {
        direction ? direction->x : 0.0f,
        direction ? direction->y : 0.0f,
        direction ? direction->z : 0.0f,
        static_cast<float>(id)
    };
    const float luminance =
        (std::max)(
            0.0f,
            color->r * 0.2126f +
            color->g * 0.7152f +
            color->b * 0.0722f);
    light.score = luminance * radius;
    if (std::isfinite(light.score) &&
        light.score > 0.0f) {
        s_frame.captured.push_back(light);
    }
}

void BeginDeferredLights() noexcept
{
    if (!SHADERENGINE_EFFECTS_ON ||
        !SHADOW_UPGRADE_ON ||
        !s_frame.collecting) {
        s_frame.captured.clear();
        s_frame.selected = {};
        s_frame.selectedCount = 0;
        s_frame.readyForComposite = false;
    }
}

void EndDeferredLights() noexcept
{
    s_frame.selected = {};
    s_frame.selectedCount = 0;
    s_frame.readyForComposite = false;

    if (SHADERENGINE_EFFECTS_ON &&
        SHADOW_UPGRADE_ON &&
        s_frame.collecting &&
        !s_frame.captured.empty()) {
        std::stable_sort(
            s_frame.captured.begin(),
            s_frame.captured.end(),
            [](const ScoredLight& a, const ScoredLight& b) {
                return a.score > b.score;
            });
        const auto count = (std::min)(
            s_frame.captured.size(),
            kMaximumTracedLights);
        for (std::size_t i = 0; i < count; ++i) {
            s_frame.selected[i] =
                s_frame.captured[i].gpu;
        }
        s_frame.selectedCount =
            static_cast<std::uint32_t>(count);
        s_frame.readyForComposite =
            s_frame.selectedCount > 0;
    }
    s_frame.collecting = false;
}

void ObserveComposite(BSRenderPassLayout* pass) noexcept
{
    if (!SHADERENGINE_EFFECTS_ON ||
        !SHADOW_UPGRADE_ON ||
        !s_frame.readyForComposite ||
        !pass ||
        !pass->shader ||
        pass->shader->shaderType != 6) {
        return;
    }

    auto* originalShader =
        g_currentOriginalPixelShader.load(std::memory_order_acquire);
    if (!originalShader) {
        return;
    }

    // Confirm that the object selected for this pass is one of the FXP tiled
    // permutations. This is structural reflection, not a compiled-code hash.
    constexpr std::uint32_t kRequiredTextures =
        (1u << kTiledDiffuseSlot) |
        (1u << kTiledSpecularSlot);
    {
        std::shared_lock shaderLock(g_ShaderDB.mutex);
        const auto shaderIt =
            g_ShaderDB.entries.find(originalShader);
        if (shaderIt == g_ShaderDB.entries.end() ||
            shaderIt->second.type != ShaderType::Pixel ||
            (shaderIt->second.textureSlotMask & kRequiredTextures) !=
                kRequiredTextures ||
            shaderIt->second.outputCount != 1) {
            return;
        }
    }

    const auto techniqueID = pass->techniqueID;
    const auto pixelShaderID =
        GetTiledPixelShaderID(techniqueID);
    std::unique_lock compositeLock(s_compositeMutex);
    s_observedComposites.insert_or_assign(
        originalShader,
        ObservedComposite{
            .techniqueID = techniqueID,
            .pixelShaderID = pixelShaderID,
        });
}

bool TryRenderLiveComposite(
    REX::W32::ID3D11DeviceContext* context,
    LiveDraw_t draw,
    const void* payload) noexcept
{
    if (!SHADERENGINE_EFFECTS_ON ||
        !SHADOW_UPGRADE_ON ||
        !TracedDeferredLightingEnabled() ||
        g_customPassRendering ||
        !s_frame.readyForComposite ||
        s_frame.selectedCount == 0 ||
        !context ||
        !draw ||
        !g_rendererData ||
        !g_rendererData->device) {
        return false;
    }

    if (!ProcessorSlotsAreAvailable()) {
        if (!s_slotConflictLogged) {
            REX::WARN(
                "TiledDeferredLighting: t11/t12/t25 conflict with "
                "configured ShaderEngine resource slots; native "
                "composite retained");
            s_slotConflictLogged = true;
        }
        return false;
    }

    auto* device = g_rendererData->device;
    SavedState saved(context);
    const auto contract = Classify(
        g_currentOriginalPixelShader.load(
            std::memory_order_acquire),
        saved.pixelShader);
    if (!contract) {
        return false;
    }

    const auto live =
        ValidateLiveContract(context, saved, *contract);
    if (!live) {
        std::lock_guard diagnosticLock(s_diagnosticMutex);
        if (s_rejectedSelections.insert(
                SelectionKey(*contract)).second) {
            REX::WARN(
                "TiledDeferredLighting: techniqueID=0x{:08X} "
                "pixelShaderID=0x{:08X} rejected live D3D11 composite "
                "(metadata={}, mainRT={}, singleRT={}, tiledInputs={}, "
                "depth={}, cb2={}, textureMask=0x{:08X}, cb2Size={}, "
                "outputs={}, t11Dimension={}, t12Dimension={}, "
                "t7Dimension={}); native composite retained",
                contract->techniqueID,
                contract->pixelShaderID,
                live.metadata,
                live.mainTarget,
                live.singleTarget,
                live.tiledInputs,
                live.depthInput,
                live.constants,
                contract->textureSlotMask,
                contract->cb2Size,
                contract->outputCount,
                static_cast<std::uint32_t>(
                    live.diffuseDimension),
                static_cast<std::uint32_t>(
                    live.specularDimension),
                static_cast<std::uint32_t>(
                    live.depthDimension));
        }
        return false;
    }

    if (!EnsureShader(device) ||
        !EnsureFixedResources(device) ||
        !EnsureScratchTargets(device, saved) ||
        !UpdateGpuInputs(context)) {
        if (!s_resourceFailureLogged) {
            REX::WARN(
                "TiledDeferredLighting: processor resources could "
                "not be prepared; native composite retained");
            s_resourceFailureLogged = true;
        }
        return false;
    }

    ScopedCustomDraw customScope;
    saved.MarkModified();
    REX::W32::ID3D11RenderTargetView* scratchRtvs[2]{
        s_scratch[0].rtv,
        s_scratch[1].rtv
    };
    context->OMSetRenderTargets(
        2, scratchRtvs, nullptr);
    context->OMSetBlendState(
        s_opaqueBlend, nullptr, 0xFFFFFFFFu);
    context->OMSetDepthStencilState(
        s_depthDisabled, 0);
    context->PSSetShader(
        s_processorShader, nullptr, 0);
    context->PSSetShaderResources(
        kCapturedLightSlot, 1, &s_lightSrv);
    context->PSSetConstantBuffers(
        kModeConstantSlot, 1, &s_modeConstant);
    if (g_customSRV) {
        context->PSSetShaderResources(
            CUSTOMBUFFER_SLOT, 1, &g_customSRV);
    }
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

    context->OMSetRenderTargets(
        static_cast<UINT>(saved.rtvs.size()),
        saved.rtvs.data(),
        saved.dsv);
    context->OMSetBlendState(
        saved.blend,
        saved.blendFactor,
        saved.sampleMask);
    context->OMSetDepthStencilState(
        saved.depthStencil,
        saved.stencilRef);
    context->PSSetShader(
        saved.pixelShader, nullptr, 0);
    REX::W32::ID3D11ShaderResourceView* processed[2]{
        s_scratch[0].srv,
        s_scratch[1].srv
    };
    context->PSSetShaderResources(
        kTiledDiffuseSlot, 2, processed);
    draw(context, payload);

    {
        std::lock_guard diagnosticLock(s_diagnosticMutex);
        if (s_acceptedSelections.insert(
                SelectionKey(*contract)).second) {
            REX::INFO(
                "TiledDeferredLighting: techniqueID=0x{:08X} "
                "pixelShaderID=0x{:08X} consumed {} captured lights "
                "through the native FXP t11/t12 permutation",
                contract->techniqueID,
                contract->pixelShaderID,
                s_frame.selectedCount);
        }
    }
    s_frame.readyForComposite = false;
    saved.Restore();
    return true;
}

void InvalidateShader() noexcept
{
    Release(s_processorShader);
    s_compileTried = false;
    s_compileFailed = false;
    s_slotConflictLogged = false;
    s_resourceFailureLogged = false;
    {
        std::lock_guard diagnosticLock(s_diagnosticMutex);
        s_acceptedSelections.clear();
        s_rejectedSelections.clear();
    }
}

void Shutdown() noexcept
{
    InvalidateShader();
    {
        std::unique_lock compositeLock(s_compositeMutex);
        s_observedComposites.clear();
    }
    for (auto& target : s_scratch) {
        target.Reset();
    }
    Release(s_opaqueBlend);
    Release(s_depthDisabled);
    Release(s_lightSrv);
    Release(s_lightBuffer);
    Release(s_modeConstant);
    s_frame = {};
}

}  // namespace TiledDeferredLighting
