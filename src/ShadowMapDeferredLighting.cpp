#include <ShadowMapDeferredLighting.h>

#include <Global.h>
#include <Plugin.h>
#include <RenderTargets.h>
#include <ShaderResources.h>
#include <ShadowUpgrade.h>

#include <array>
#include <cstring>
#include <fstream>

namespace ShadowMapDeferredLighting {
namespace {

// The original pixel-shader assembly identities below are the complete OG
// shadowed-local-light contract set found in the shipped ShaderDB:
//
//   planar/projector: single compare, 3x3 PCF, 16-tap disk PCF
//   dual paraboloid:  single compare, 3x3 PCF, 16-tap disk PCF
//
// The local-light path runs in both interiors and exteriors. Directional
// exterior sunlight remains owned by pixelShadowVisibilityOG.
enum class Contract : std::uint32_t
{
    kPlanarSingle = 0,
    kPlanarGrid3 = 1,
    kPlanarDisk16 = 2,
    kPointSingle = 3,
    kPointGrid3 = 4,
    kPointDisk16 = 5,
    kCount
};

constexpr std::array<std::pair<std::uint32_t, Contract>, 51> kContracts{ {
    { 0xA111C744u, Contract::kPlanarSingle },
    { 0x0F2D4202u, Contract::kPlanarSingle },
    { 0x1F61FA95u, Contract::kPlanarSingle },
    { 0x465890EEu, Contract::kPlanarSingle },
    { 0x719281FBu, Contract::kPlanarSingle },
    { 0x88C450BFu, Contract::kPlanarSingle },
    { 0xC4B36E5Du, Contract::kPlanarSingle },
    { 0xFC2CE83Fu, Contract::kPlanarSingle },
    { 0x4E6D6ED4u, Contract::kPlanarGrid3 },
    { 0x28435829u, Contract::kPlanarGrid3 },
    { 0x2973F143u, Contract::kPlanarGrid3 },
    { 0x2D5E329Bu, Contract::kPlanarGrid3 },
    { 0x42C2755Eu, Contract::kPlanarGrid3 },
    { 0xA4719509u, Contract::kPlanarGrid3 },
    { 0xDBDA6EAFu, Contract::kPlanarGrid3 },
    { 0xE044A69Eu, Contract::kPlanarGrid3 },
    { 0xE1814A65u, Contract::kPlanarDisk16 },
    { 0x70426686u, Contract::kPlanarDisk16 },
    { 0x256D2BBAu, Contract::kPlanarDisk16 },
    { 0x3AE2D61Du, Contract::kPlanarDisk16 },
    { 0x7F898813u, Contract::kPlanarDisk16 },
    { 0xB776A216u, Contract::kPlanarDisk16 },
    { 0xFB1B3AB0u, Contract::kPlanarDisk16 },
    { 0x5B279219u, Contract::kPointSingle },
    { 0x009E3E8Au, Contract::kPointSingle },
    { 0x05362F69u, Contract::kPointSingle },
    { 0x123371C1u, Contract::kPointSingle },
    { 0x13807EACu, Contract::kPointSingle },
    { 0x3CB47781u, Contract::kPointSingle },
    { 0x58FF71B4u, Contract::kPointSingle },
    { 0x7A9B8CA4u, Contract::kPointSingle },
    { 0x93E6F049u, Contract::kPointSingle },
    { 0xA905C891u, Contract::kPointGrid3 },
    { 0x583F47B9u, Contract::kPointGrid3 },
    { 0x64528CC3u, Contract::kPointGrid3 },
    { 0x64AB165Eu, Contract::kPointGrid3 },
    { 0x77FC526Au, Contract::kPointGrid3 },
    { 0x78B42621u, Contract::kPointGrid3 },
    { 0xA71E4470u, Contract::kPointGrid3 },
    { 0xE2277D0Cu, Contract::kPointGrid3 },
    { 0xE66FCA31u, Contract::kPointGrid3 },
    { 0xF586DEE1u, Contract::kPointGrid3 },
    { 0xF72B8744u, Contract::kPointGrid3 },
    { 0x0BF3C87Eu, Contract::kPointDisk16 },
    { 0x04A6CA90u, Contract::kPointDisk16 },
    { 0x0C99D3E6u, Contract::kPointDisk16 },
    { 0x1C573857u, Contract::kPointDisk16 },
    { 0x24551FFCu, Contract::kPointDisk16 },
    { 0x784A2D42u, Contract::kPointDisk16 },
    { 0x914F6F21u, Contract::kPointDisk16 },
    { 0xCD03DFFDu, Contract::kPointDisk16 },
} };

constexpr UINT kScratchSrvSlot = 28;
constexpr UINT kModeConstantSlot = 13;
constexpr char kShaderFile[] = "ShadowMapDeferredLighting.hlsl";

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
    std::array<REX::W32::ID3D11ShaderResourceView*, 2> scratchSlots{};
    std::array<REX::W32::ID3D11ShaderResourceView*, 3> modularSlots{};
    REX::W32::ID3D11Buffer* modeConstant = nullptr;
    bool captured = false;

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
    }

    void Restore() noexcept
    {
        if (!captured || !context) {
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
        context->PSSetShader(pixelShader, nullptr, 0);
        context->OMSetDepthStencilState(depthStencil, stencilRef);
        context->OMSetBlendState(blend, blendFactor, sampleMask);
        context->OMSetRenderTargets(
            static_cast<UINT>(rtvs.size()), rtvs.data(), dsv);
        captured = false;
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

std::array<ScratchTarget, 2> s_scratch{};
REX::W32::ID3D11PixelShader* s_filterShader = nullptr;
REX::W32::ID3D11BlendState* s_opaqueBlend = nullptr;
REX::W32::ID3D11BlendState* s_additiveBlend = nullptr;
std::array<REX::W32::ID3D11Buffer*,
           static_cast<std::size_t>(Contract::kCount)> s_modeConstants{};
bool s_compileTried = false;
bool s_compileFailed = false;
bool s_firstFireLogged = false;
bool s_contractSkipLogged = false;
bool s_slotConflictLogged = false;

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

std::optional<Contract> Classify(
    REX::W32::ID3D11PixelShader* originalShader,
    REX::W32::ID3D11PixelShader* activeShader) noexcept
{
    if (!originalShader || !activeShader) {
        return std::nullopt;
    }

    std::uint32_t asmHash = 0;
    REX::W32::ID3D11PixelShader* replacementShader = nullptr;
    {
        std::shared_lock lock(g_ShaderDB.mutex);
        const auto it = g_ShaderDB.entries.find(originalShader);
        if (it == g_ShaderDB.entries.end()) {
            return std::nullopt;
        }
        asmHash = it->second.asmHash;
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

    for (const auto& [hash, contract] : kContracts) {
        if (asmHash == hash) {
            return contract;
        }
    }
    return std::nullopt;
}

bool ScratchSlotsAreAvailable() noexcept
{
    constexpr std::array<UINT, 2> scratch{
        kScratchSrvSlot, kScratchSrvSlot + 1
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
        if (value && value->id == "ps_GIEnabled") {
            return value->current.b;
        }
    }
    // The renderer feature is still useful without a HachiToon Values.ini.
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
    if (s_filterShader) {
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
            "ShadowMapDeferredLighting: shader file not found: {}",
            path.string());
        s_compileFailed = true;
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
            "ShadowMapDeferredLighting",
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
            s_compileFailed = true;
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
        &s_filterShader);
    g_isCreatingReplacementShader = previousCreationFlag;
    const auto byteCount = blob->GetBufferSize();
    Release(blob);

    if (!REX::W32::SUCCESS(hr) || !s_filterShader) {
        REX::WARN(
            "ShadowMapDeferredLighting: CreatePixelShader failed 0x{:08X}",
            static_cast<unsigned>(hr));
        s_compileFailed = true;
        return false;
    }

    REX::INFO(
        "ShadowMapDeferredLighting: compiled map-space PCSS composite "
        "({} bytes)",
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

    if (!s_additiveBlend) {
        REX::W32::D3D11_BLEND_DESC desc{};
        auto& target = desc.renderTarget[0];
        target.blendEnable = true;
        target.srcBlend = REX::W32::D3D11_BLEND_ONE;
        target.destBlend = REX::W32::D3D11_BLEND_ONE;
        target.blendOp = REX::W32::D3D11_BLEND_OP_ADD;
        target.srcBlendAlpha = REX::W32::D3D11_BLEND_ONE;
        target.destBlendAlpha = REX::W32::D3D11_BLEND_ONE;
        target.blendOpAlpha = REX::W32::D3D11_BLEND_OP_ADD;
        target.renderTargetWriteMask =
            REX::W32::D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&desc, &s_additiveBlend);
        if (!REX::W32::SUCCESS(hr)) {
            REX::WARN(
                "ShadowMapDeferredLighting: additive blend creation failed "
                "0x{:08X}",
                static_cast<unsigned>(hr));
            return false;
        }
    }

    REX::W32::D3D11_BUFFER_DESC desc{};
    desc.byteWidth = 16;
    desc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
    desc.bindFlags = REX::W32::D3D11_BIND_CONSTANT_BUFFER;
    for (std::size_t i = 0; i < s_modeConstants.size(); ++i) {
        if (s_modeConstants[i]) {
            continue;
        }
        const std::array<std::uint32_t, 4> mode{
            static_cast<std::uint32_t>(i), 0, 0, 0
        };
        REX::W32::D3D11_SUBRESOURCE_DATA initial{};
        initial.sysMem = mode.data();
        hr = device->CreateBuffer(
            &desc, &initial, &s_modeConstants[i]);
        if (!REX::W32::SUCCESS(hr)) {
            REX::WARN(
                "ShadowMapDeferredLighting: mode constant {} creation "
                "failed 0x{:08X}",
                i,
                static_cast<unsigned>(hr));
            return false;
        }
    }
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
    if (!CreateScratchTarget(
            device,
            g_rendererData->renderTargets[diffuse],
            s_scratch[0]) ||
        !CreateScratchTarget(
            device,
            g_rendererData->renderTargets[specular],
            s_scratch[1])) {
        REX::WARN(
            "ShadowMapDeferredLighting: failed to mirror BGS deferred MRTs");
        return false;
    }
    return true;
}

struct LiveContract
{
    bool mrts = false;
    bool srvs = false;
    bool shadowViews = false;
    bool constants = false;
    bool sampler = false;
    bool readOnlyDepthStencil = false;

    explicit operator bool() const noexcept
    {
        return mrts && srvs && shadowViews && constants && sampler &&
            readOnlyDepthStencil;
    }
};

LiveContract ValidateLiveContract(
    REX::W32::ID3D11DeviceContext* context,
    const SavedState& saved) noexcept
{
    LiveContract result{};
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

    // Both rasterizations must cover the same pixels. The first writes the
    // isolated light into scratch; the second reads it back. Replaying the
    // live depth/stencil state is safe only when that state cannot mutate the
    // underlying depth/stencil surface on the first draw.
    result.readOnlyDepthStencil = !saved.dsv;
    if (saved.dsv && saved.depthStencil) {
        REX::W32::D3D11_DEPTH_STENCIL_DESC desc{};
        saved.depthStencil->GetDesc(&desc);
        const bool depthWrites =
            desc.depthEnable &&
            desc.depthWriteMask !=
                REX::W32::D3D11_DEPTH_WRITE_MASK_ZERO;
        bool stencilWrites = false;
        if (desc.stencilEnable && desc.stencilWriteMask != 0) {
            const auto mutates =
                [](const REX::W32::D3D11_DEPTH_STENCILOP_DESC& face) {
                    return
                        face.stencilFailOp !=
                            REX::W32::D3D11_STENCIL_OP_KEEP ||
                        face.stencilDepthFailOp !=
                            REX::W32::D3D11_STENCIL_OP_KEEP ||
                        face.stencilPassOp !=
                            REX::W32::D3D11_STENCIL_OP_KEEP;
                };
            stencilWrites =
                mutates(desc.frontFace) || mutates(desc.backFace);
        }
        result.readOnlyDepthStencil = !depthWrites && !stencilWrites;
    }

    std::array<REX::W32::ID3D11ShaderResourceView*, 4> srvs{};
    context->PSGetShaderResources(2, 4, srvs.data());
    std::array<REX::W32::ID3D11Buffer*, 2> constants{};
    context->PSGetConstantBuffers(2, 1, &constants[0]);
    context->PSGetConstantBuffers(12, 1, &constants[1]);
    REX::W32::ID3D11SamplerState* comparisonSampler = nullptr;
    context->PSGetSamplers(5, 1, &comparisonSampler);

    result.srvs = srvs[0] && srvs[1] && srvs[2] && srvs[3];
    result.constants = constants[0] && constants[1];
    result.sampler = comparisonSampler != nullptr;

    if (srvs[2] && srvs[3]) {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC rawDesc{};
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC compareDesc{};
        srvs[2]->GetDesc(&rawDesc);
        srvs[3]->GetDesc(&compareDesc);

        REX::W32::ID3D11Resource* rawResource = nullptr;
        REX::W32::ID3D11Resource* compareResource = nullptr;
        srvs[2]->GetResource(&rawResource);
        srvs[3]->GetResource(&compareResource);
        result.shadowViews =
            rawResource && rawResource == compareResource &&
            rawDesc.viewDimension ==
                REX::W32::D3D11_SRV_DIMENSION_TEXTURE2DARRAY &&
            compareDesc.viewDimension ==
                REX::W32::D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        Release(rawResource);
        Release(compareResource);
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

bool TryRender(
    BSRenderPassLayout* pass,
    std::uintptr_t arg2,
    std::uintptr_t arg3,
    RE::BSGraphics::DynamicTriShapeDrawData* dynamicDrawData,
    Draw_t draw) noexcept
{
    if (!SHADERENGINE_EFFECTS_ON ||
        !SHADOW_UPGRADE_ON ||
        !ShadowUpgrade::IsInDeferredLighting() ||
        !TracedDeferredLightingEnabled() ||
        !pass ||
        !draw ||
        !g_rendererData ||
        !g_rendererData->device ||
        !g_rendererData->context) {
        return false;
    }

    auto* device = g_rendererData->device;
    auto* context = g_rendererData->context;
    if (!ScratchSlotsAreAvailable()) {
        if (!s_slotConflictLogged) {
            REX::WARN(
                "ShadowMapDeferredLighting: scratch SRV slots t28/t29 "
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

    const auto liveContract = ValidateLiveContract(context, saved);
    if (!liveContract) {
        if (!s_contractSkipLogged) {
            REX::WARN(
                "ShadowMapDeferredLighting: recognized local-shadow shader "
                "rejected live BGS contract (MRTs={}, SRVs={}, "
                "shadowViews={}, CBs={}, sampler={}, readOnlyDS={}); "
                "vanilla draw retained",
                liveContract.mrts,
                liveContract.srvs,
                liveContract.shadowViews,
                liveContract.constants,
                liveContract.sampler,
                liveContract.readOnlyDepthStencil);
            s_contractSkipLogged = true;
        }
        return false;
    }

    if (!EnsureShader(device) ||
        !EnsureFixedStates(device) ||
        !EnsureScratchTargets(device)) {
        return false;
    }

    ScopedCustomDraw customScope;

    // Isolate the exact output of this one BGS light. Opaque scratch blending
    // avoids coupling the upgraded visibility to previously accumulated lights.
    REX::W32::ID3D11RenderTargetView* scratchRtvs[2]{
        s_scratch[0].rtv,
        s_scratch[1].rtv
    };
    context->OMSetRenderTargets(2, scratchRtvs, saved.dsv);
    context->OMSetBlendState(s_opaqueBlend, nullptr, 0xFFFFFFFFu);
    draw(pass, arg2, arg3, dynamicDrawData);

    // Re-rasterize the same light volume under the validated read-only
    // depth/stencil state. Every fragment that can read scratch was overwritten
    // by the opaque first draw, so no full-screen scratch clear is required.
    context->OMSetRenderTargets(
        static_cast<UINT>(saved.rtvs.size()),
        saved.rtvs.data(),
        saved.dsv);
    context->OMSetBlendState(s_additiveBlend, nullptr, 0xFFFFFFFFu);
    context->PSSetShader(s_filterShader, nullptr, 0);

    REX::W32::ID3D11ShaderResourceView* scratchSrvs[2]{
        s_scratch[0].srv,
        s_scratch[1].srv
    };
    context->PSSetShaderResources(kScratchSrvSlot, 2, scratchSrvs);
    auto* modeConstant =
        s_modeConstants[static_cast<std::size_t>(*contract)];
    context->PSSetConstantBuffers(
        kModeConstantSlot, 1, &modeConstant);

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

    draw(pass, arg2, arg3, dynamicDrawData);

    if (!s_firstFireLogged) {
        REX::INFO(
            "ShadowMapDeferredLighting: first shadowed local light isolated "
            "and recomposited (contract={})",
            static_cast<std::uint32_t>(*contract));
        s_firstFireLogged = true;
    }
    // Restore while custom rendering suppression is still active. Otherwise
    // the PSSetShader hook would mistake the saved replacement object for a
    // new engine/original shader and corrupt the current ShaderDB identity.
    saved.Restore();
    return true;
}

void InvalidateShader() noexcept
{
    Release(s_filterShader);
    s_compileTried = false;
    s_compileFailed = false;
    s_firstFireLogged = false;
    s_contractSkipLogged = false;
    s_slotConflictLogged = false;
}

void Shutdown() noexcept
{
    InvalidateShader();
    for (auto& target : s_scratch) {
        target.Reset();
    }
    Release(s_opaqueBlend);
    Release(s_additiveBlend);
    for (auto*& constant : s_modeConstants) {
        Release(constant);
    }
}

}  // namespace ShadowMapDeferredLighting
