#include <Global.h>
#include <Plugin.h>
#include <d3dhooks.h>
#include <WaterTessellation.h>

#include <limits>

namespace WaterTessellation
{
namespace
{
    struct Pipeline
    {
        REX::W32::ID3D11VertexShader* vertex = nullptr;
        REX::W32::ID3D11HullShader* hull = nullptr;
        REX::W32::ID3D11DomainShader* domain = nullptr;
        REX::W32::ID3D11PixelShader* debugPixel = nullptr;
        REX::W32::ID3D11RasterizerState* sourceRaster = nullptr;
        REX::W32::ID3D11RasterizerState* noCullRaster = nullptr;
        REX::W32::ID3D11RasterizerState* wireframeRaster = nullptr;
        std::uint64_t manualReloadGeneration = (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t requestedReloadGeneration = (std::numeric_limits<std::uint64_t>::max)();
        bool compileFailed = false;
    };

    Pipeline g_pipeline;
    std::mutex g_pipelineMutex;
    std::atomic_uint64_t g_reloadRequests{ 0 };

    void ReleaseRasterizersLocked()
    {
        if (g_pipeline.sourceRaster) g_pipeline.sourceRaster->Release();
        if (g_pipeline.noCullRaster) g_pipeline.noCullRaster->Release();
        if (g_pipeline.wireframeRaster) g_pipeline.wireframeRaster->Release();
        g_pipeline.sourceRaster = nullptr;
        g_pipeline.noCullRaster = nullptr;
        g_pipeline.wireframeRaster = nullptr;
    }

    void ReleasePipelineLocked()
    {
        if (g_pipeline.vertex) g_pipeline.vertex->Release();
        if (g_pipeline.hull) g_pipeline.hull->Release();
        if (g_pipeline.domain) g_pipeline.domain->Release();
        if (g_pipeline.debugPixel) g_pipeline.debugPixel->Release();
        g_pipeline.vertex = nullptr;
        g_pipeline.hull = nullptr;
        g_pipeline.domain = nullptr;
        g_pipeline.debugPixel = nullptr;
        ReleaseRasterizersLocked();
        g_pipeline.compileFailed = false;
    }

    bool FindBool(std::string_view id, bool fallback)
    {
        for (const auto* value : g_shaderSettings.GetBoolShaderValues()) {
            if (value && value->id == id) return value->current.b;
        }
        return fallback;
    }

    int FindInt(std::string_view id, int fallback)
    {
        for (const auto* value : g_shaderSettings.GetIntShaderValues()) {
            if (value && value->id == id) return value->current.i;
        }
        return fallback;
    }

    std::string BuildShaderHeader()
    {
        std::string header = GetCommonShaderHeaderHLSLTop();
        header += GetCommonShaderHeaderHLSLBottom();
        header += "// shader settings named accessors\n";
        constexpr std::array<std::string_view, 4> swizzle{ ".x", ".y", ".z", ".w" };
        for (auto* value : g_shaderSettings.GetFloatShaderValues()) {
            header += std::format(
                "#define {} GFXModularFloats[{}]{}\n",
                value->id, value->bufferIndex / 4, swizzle[value->bufferIndex % 4]);
        }
        for (auto* value : g_shaderSettings.GetIntShaderValues()) {
            header += std::format(
                "#define {} GFXModularInts[{}]{}\n",
                value->id, value->bufferIndex / 4, swizzle[value->bufferIndex % 4]);
        }
        for (auto* value : g_shaderSettings.GetBoolShaderValues()) {
            header += std::format(
                "#define {} (GFXModularBools[{}]{} != 0)\n",
                value->id, value->bufferIndex / 4, swizzle[value->bufferIndex % 4]);
        }
        return header + "\n";
    }

    bool CompileStage(
        const std::string& source,
        const std::filesystem::path& sourcePath,
        std::string_view entry,
        std::string_view profile,
        ID3DBlob** output)
    {
        constexpr UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        const auto key = ShaderCache::ComputeKey({
            .assembledSource = source,
            .profile = profile,
            .entry = entry,
            .flags = flags,
            .shaderFolder = sourcePath.parent_path(),
        });
        if (ShaderCache::TryLoad(key, output)) return true;

        ID3DBlob* errors = nullptr;
        ShaderIncludeHandler includer;
        const HRESULT hr = D3DCompile(
            source.data(), source.size(), sourcePath.string().c_str(), nullptr,
            &includer, entry.data(), profile.data(), flags, 0, output, &errors);
        if (FAILED(hr)) {
            REX::WARN(
                "WaterTessellation: {} {} compilation failed: {}",
                entry, profile,
                errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
            if (errors) errors->Release();
            return false;
        }
        if (errors) errors->Release();
        ShaderCache::Store(key, *output);
        return true;
    }

    bool EnsurePipelineLocked(REX::W32::ID3D11Device* device)
    {
        const auto manualGeneration =
            g_manualShaderReloadGeneration.load(std::memory_order_acquire);
        const auto requestedGeneration = g_reloadRequests.load(std::memory_order_acquire);
        if (g_pipeline.manualReloadGeneration != manualGeneration ||
            g_pipeline.requestedReloadGeneration != requestedGeneration) {
            ReleasePipelineLocked();
            g_pipeline.manualReloadGeneration = manualGeneration;
            g_pipeline.requestedReloadGeneration = requestedGeneration;
        }
        if (g_pipeline.vertex && g_pipeline.hull && g_pipeline.domain &&
            g_pipeline.debugPixel) {
            return true;
        }
        if (g_pipeline.compileFailed || !device) return false;

        const auto sourcePath = g_shaderFolderPath / "HachiToon" / "waterTessellationO6.hlsl";
        std::ifstream file(sourcePath, std::ios::binary);
        if (!file.good()) {
            REX::WARN("WaterTessellation: shader source not found: {}", sourcePath.string());
            g_pipeline.compileFailed = true;
            return false;
        }
        std::string body{
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
        const std::string source = BuildShaderHeader() + body;

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* hsBlob = nullptr;
        ID3DBlob* dsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        const bool compiled =
            CompileStage(source, sourcePath, "vsMain", "vs_5_0", &vsBlob) &&
            CompileStage(source, sourcePath, "hsMain", "hs_5_0", &hsBlob) &&
            CompileStage(source, sourcePath, "dsMain", "ds_5_0", &dsBlob) &&
            CompileStage(source, sourcePath, "debugWireframePS", "ps_5_0", &psBlob);
        if (!compiled) {
            if (vsBlob) vsBlob->Release();
            if (hsBlob) hsBlob->Release();
            if (dsBlob) dsBlob->Release();
            if (psBlob) psBlob->Release();
            g_pipeline.compileFailed = true;
            return false;
        }

        g_isCreatingReplacementShader = true;
        const HRESULT vsResult = device->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_pipeline.vertex);
        const HRESULT hsResult = device->CreateHullShader(
            hsBlob->GetBufferPointer(), hsBlob->GetBufferSize(), nullptr, &g_pipeline.hull);
        const HRESULT dsResult = device->CreateDomainShader(
            dsBlob->GetBufferPointer(), dsBlob->GetBufferSize(), nullptr, &g_pipeline.domain);
        const HRESULT psResult = device->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_pipeline.debugPixel);
        g_isCreatingReplacementShader = false;
        vsBlob->Release();
        hsBlob->Release();
        dsBlob->Release();
        psBlob->Release();

        if (FAILED(vsResult) || FAILED(hsResult) || FAILED(dsResult) || FAILED(psResult)) {
            REX::WARN(
                "WaterTessellation: shader creation failed "
                "(VS=0x{:08X}, HS=0x{:08X}, DS=0x{:08X}, PS=0x{:08X})",
                static_cast<unsigned>(vsResult), static_cast<unsigned>(hsResult),
                static_cast<unsigned>(dsResult), static_cast<unsigned>(psResult));
            ReleasePipelineLocked();
            g_pipeline.compileFailed = true;
            return false;
        }
        REX::INFO("WaterTessellation: O6 VS/HS/DS/debug-PS pipeline compiled and ready");
        return true;
    }

    bool EnsureRasterizersLocked(
        REX::W32::ID3D11Device* device,
        REX::W32::ID3D11RasterizerState* sourceRaster)
    {
        if (g_pipeline.sourceRaster == sourceRaster &&
            g_pipeline.noCullRaster && g_pipeline.wireframeRaster) {
            return true;
        }

        ReleaseRasterizersLocked();
        REX::W32::D3D11_RASTERIZER_DESC desc{};
        if (sourceRaster) {
            sourceRaster->GetDesc(&desc);
            sourceRaster->AddRef();
            g_pipeline.sourceRaster = sourceRaster;
        } else {
            // D3D11's implicit default rasterizer state.
            desc.fillMode = REX::W32::D3D11_FILL_SOLID;
            desc.cullMode = REX::W32::D3D11_CULL_BACK;
            desc.frontCounterClockwise = false;
            desc.depthClipEnable = true;
        }

        desc.fillMode = REX::W32::D3D11_FILL_SOLID;
        desc.cullMode = REX::W32::D3D11_CULL_NONE;
        const HRESULT solidResult =
            device->CreateRasterizerState(&desc, &g_pipeline.noCullRaster);
        desc.fillMode = REX::W32::D3D11_FILL_WIREFRAME;
        desc.cullMode = REX::W32::D3D11_CULL_NONE;
        const HRESULT wireResult =
            device->CreateRasterizerState(&desc, &g_pipeline.wireframeRaster);
        if (FAILED(solidResult) || FAILED(wireResult)) {
            REX::WARN(
                "WaterTessellation: rasterizer creation failed "
                "(solid=0x{:08X}, wire=0x{:08X})",
                static_cast<unsigned>(solidResult), static_cast<unsigned>(wireResult));
            ReleaseRasterizersLocked();
            return false;
        }
        return true;
    }

    struct PipelineSnapshot
    {
        REX::W32::ID3D11VertexShader* vertex = nullptr;
        REX::W32::ID3D11HullShader* hull = nullptr;
        REX::W32::ID3D11DomainShader* domain = nullptr;
        REX::W32::ID3D11PixelShader* debugPixel = nullptr;
        REX::W32::ID3D11RasterizerState* noCullRaster = nullptr;
        REX::W32::ID3D11RasterizerState* wireframeRaster = nullptr;

        void Release()
        {
            if (vertex) vertex->Release();
            if (hull) hull->Release();
            if (domain) domain->Release();
            if (debugPixel) debugPixel->Release();
            if (noCullRaster) noCullRaster->Release();
            if (wireframeRaster) wireframeRaster->Release();
            *this = {};
        }
    };

    bool AcquirePipeline(
        REX::W32::ID3D11Device* device,
        REX::W32::ID3D11RasterizerState* sourceRaster,
        PipelineSnapshot& snapshot)
    {
        std::scoped_lock lock(g_pipelineMutex);
        if (!EnsurePipelineLocked(device) ||
            !EnsureRasterizersLocked(device, sourceRaster)) {
            return false;
        }

        snapshot.vertex = g_pipeline.vertex;
        snapshot.hull = g_pipeline.hull;
        snapshot.domain = g_pipeline.domain;
        snapshot.debugPixel = g_pipeline.debugPixel;
        snapshot.noCullRaster = g_pipeline.noCullRaster;
        snapshot.wireframeRaster = g_pipeline.wireframeRaster;
        snapshot.vertex->AddRef();
        snapshot.hull->AddRef();
        snapshot.domain->AddRef();
        snapshot.debugPixel->AddRef();
        snapshot.noCullRaster->AddRef();
        snapshot.wireframeRaster->AddRef();
        return true;
    }

    struct StageResources
    {
        std::array<REX::W32::ID3D11ShaderResourceView*, 4> hs{};
        std::array<REX::W32::ID3D11ShaderResourceView*, 4> ds{};
        std::array<UINT, 4> slots{};
    };

    StageResources BindInjectedResources(REX::W32::ID3D11DeviceContext* context)
    {
        StageResources saved{};
        saved.slots = { CUSTOMBUFFER_SLOT, MODULAR_FLOATS_SLOT, MODULAR_INTS_SLOT, MODULAR_BOOLS_SLOT };
        const std::array<REX::W32::ID3D11ShaderResourceView*, 4> resources{
            g_customSRV, g_modularFloatsSRV, g_modularIntsSRV, g_modularBoolsSRV
        };
        for (std::size_t i = 0; i < saved.slots.size(); ++i) {
            context->HSGetShaderResources(saved.slots[i], 1, &saved.hs[i]);
            context->DSGetShaderResources(saved.slots[i], 1, &saved.ds[i]);
            context->HSSetShaderResources(saved.slots[i], 1, &resources[i]);
            context->DSSetShaderResources(saved.slots[i], 1, &resources[i]);
        }
        return saved;
    }

    void RestoreInjectedResources(
        REX::W32::ID3D11DeviceContext* context,
        StageResources& saved)
    {
        for (std::size_t i = 0; i < saved.slots.size(); ++i) {
            context->HSSetShaderResources(saved.slots[i], 1, &saved.hs[i]);
            context->DSSetShaderResources(saved.slots[i], 1, &saved.ds[i]);
            if (saved.hs[i]) saved.hs[i]->Release();
            if (saved.ds[i]) saved.ds[i]->Release();
        }
    }

    struct StageConstantBuffers
    {
        std::array<REX::W32::ID3D11Buffer*, 3> vertex{};
        std::array<REX::W32::ID3D11Buffer*, 3> hull{};
        std::array<REX::W32::ID3D11Buffer*, 3> domain{};
        std::array<UINT, 3> slots{ 1, 2, 12 };
    };

    StageConstantBuffers BindWaterConstantBuffers(REX::W32::ID3D11DeviceContext* context)
    {
        StageConstantBuffers saved{};
        for (std::size_t i = 0; i < saved.slots.size(); ++i) {
            context->VSGetConstantBuffers(saved.slots[i], 1, &saved.vertex[i]);
            context->HSGetConstantBuffers(saved.slots[i], 1, &saved.hull[i]);
            context->DSGetConstantBuffers(saved.slots[i], 1, &saved.domain[i]);
            context->HSSetConstantBuffers(saved.slots[i], 1, &saved.vertex[i]);
            context->DSSetConstantBuffers(saved.slots[i], 1, &saved.vertex[i]);
        }
        return saved;
    }

    void RestoreWaterConstantBuffers(
        REX::W32::ID3D11DeviceContext* context,
        StageConstantBuffers& saved)
    {
        for (std::size_t i = 0; i < saved.slots.size(); ++i) {
            context->HSSetConstantBuffers(saved.slots[i], 1, &saved.hull[i]);
            context->DSSetConstantBuffers(saved.slots[i], 1, &saved.domain[i]);
            if (saved.vertex[i]) saved.vertex[i]->Release();
            if (saved.hull[i]) saved.hull[i]->Release();
            if (saved.domain[i]) saved.domain[i]->Release();
        }
    }

    bool MatchesProvenO6Contract(
        REX::W32::ID3D11DeviceContext* context,
        UINT indexCount,
        REX::W32::ID3D11VertexShader** selectedShader)
    {
        auto* originalPS = g_currentOriginalPixelShader.load(std::memory_order_acquire);
        auto* pixelDefinition = originalPS ? g_ShaderDB.GetMatchedDefinition(originalPS) : nullptr;
        if (!pixelDefinition || pixelDefinition->id != "visualWater") return false;

        // The tracker is process-wide, while a D3D11 device can expose more
        // than one context.  Prove that this context actually has the expected
        // physical-water replacement bound before changing its topology.
        auto* expectedPixel = g_ShaderDB.GetReplacementShader(originalPS);
        REX::W32::ID3D11PixelShader* boundPixel = nullptr;
        UINT pixelClasses = 0;
        context->PSGetShader(&boundPixel, nullptr, &pixelClasses);
        const bool pixelMatches =
            expectedPixel && boundPixel == expectedPixel && pixelClasses == 0;
        if (boundPixel) boundPixel->Release();
        if (!pixelMatches) return false;

        auto* originalVS = GetCurrentOriginalVertexShader_Internal();
        auto* selectedVS = GetCurrentSelectedVertexShader_Internal();
        auto* vertexDefinition = originalVS ? g_ShaderDB.GetMatchedDefinition(originalVS) : nullptr;
        auto* expectedReplacement = originalVS ? g_ShaderDB.GetReplacementShader(originalVS) : nullptr;
        if (!vertexDefinition || vertexDefinition->id != "waterVertexDumpO6" ||
            g_ShaderDB.GetShaderUID(originalVS) != "VSD0439849I2O6" ||
            !expectedReplacement || selectedVS != expectedReplacement) {
            return false;
        }
        // Runtime telemetry proved the visible O6 surface as one indexed quad:
        // two triangles, six R16 indices. Do not generalize beyond that evidence.
        if (indexCount != 6) return false;

        REX::W32::D3D11_PRIMITIVE_TOPOLOGY topology{};
        context->IAGetPrimitiveTopology(&topology);
        if (topology != REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) return false;

        REX::W32::ID3D11Buffer* vertexBuffer = nullptr;
        UINT stride = 0;
        UINT vertexOffset = 0;
        context->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &vertexOffset);
        if (vertexBuffer) vertexBuffer->Release();
        if (stride != 20) return false;

        REX::W32::ID3D11Buffer* indexBuffer = nullptr;
        REX::W32::DXGI_FORMAT indexFormat = REX::W32::DXGI_FORMAT_UNKNOWN;
        UINT indexOffset = 0;
        context->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
        if (indexBuffer) indexBuffer->Release();
        if (indexFormat != REX::W32::DXGI_FORMAT_R16_UINT) return false;

        REX::W32::ID3D11HullShader* hull = nullptr;
        REX::W32::ID3D11DomainShader* domain = nullptr;
        UINT vsClasses = 0;
        context->VSGetShader(selectedShader, nullptr, &vsClasses);
        context->HSGetShader(&hull, nullptr, nullptr);
        context->DSGetShader(&domain, nullptr, nullptr);
        const bool matches = *selectedShader == selectedVS && vsClasses == 0 && !hull && !domain;
        if (hull) hull->Release();
        if (domain) domain->Release();
        if (!matches && *selectedShader) {
            (*selectedShader)->Release();
            *selectedShader = nullptr;
        }
        return matches;
    }
}

void RequestReload()
{
    g_reloadRequests.fetch_add(1, std::memory_order_release);
}

bool TryDrawIndexed(
    REX::W32::ID3D11DeviceContext* context,
    UINT indexCount,
    UINT startIndexLocation,
    INT baseVertexLocation)
{
    if (!context || !SHADERENGINE_EFFECTS_ON ||
        !FindBool("vu_WaterPhysicalEnabled", false) ||
        !FindBool("vu_WaterDisplacementEnabled", false) ||
        !FindBool("vu_WaterTessellationEnabled", false)) {
        return false;
    }

    REX::W32::ID3D11VertexShader* selectedShader = nullptr;
    if (!MatchesProvenO6Contract(context, indexCount, &selectedShader)) return false;

    REX::W32::ID3D11RasterizerState* originalRaster = nullptr;
    context->RSGetState(&originalRaster);
    REX::W32::ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    PipelineSnapshot pipeline{};
    if (!AcquirePipeline(device, originalRaster, pipeline)) {
        if (device) device->Release();
        if (originalRaster) originalRaster->Release();
        selectedShader->Release();
        return false;
    }
    device->Release();

    const bool wireframe = FindInt("vu_WaterTessellationDebugView", 0) == 1;
    REX::W32::ID3D11PixelShader* originalPixel = nullptr;
    if (wireframe) context->PSGetShader(&originalPixel, nullptr, nullptr);

    auto resources = BindInjectedResources(context);
    auto constantBuffers = BindWaterConstantBuffers(context);
    D3D11Hooks::OriginalRSSetState(
        context,
        wireframe ? pipeline.wireframeRaster : pipeline.noCullRaster);
    if (wireframe) {
        D3D11Hooks::OriginalPSSetShader(context, pipeline.debugPixel, nullptr, 0);
    }
    D3D11Hooks::OriginalVSSetShader(context, pipeline.vertex, nullptr, 0);
    context->HSSetShader(pipeline.hull, nullptr, 0);
    context->DSSetShader(pipeline.domain, nullptr, 0);
    context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

    D3D11Hooks::OriginalDrawIndexed(
        context, indexCount, startIndexLocation, baseVertexLocation);

    context->IASetPrimitiveTopology(REX::W32::D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->DSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    D3D11Hooks::OriginalVSSetShader(context, selectedShader, nullptr, 0);
    if (wireframe) {
        D3D11Hooks::OriginalPSSetShader(context, originalPixel, nullptr, 0);
    }
    D3D11Hooks::OriginalRSSetState(context, originalRaster);
    RestoreWaterConstantBuffers(context, constantBuffers);
    RestoreInjectedResources(context, resources);
    if (originalPixel) originalPixel->Release();
    if (originalRaster) originalRaster->Release();
    pipeline.Release();
    selectedShader->Release();

    if (DEVELOPMENT) {
        static std::atomic_bool logged{ false };
        if (!logged.exchange(true, std::memory_order_acq_rel)) {
            REX::INFO(
                "WaterTessellationDiag: O6 visualWater draw tessellated; "
                "indexCount={} patches={} stride=20 indexFormat=R16_UINT wireframe={}",
                indexCount, indexCount / 3, wireframe);
        }
    }
    return true;
}
}
