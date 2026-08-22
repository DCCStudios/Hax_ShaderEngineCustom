#include <PCH.h>
#include <Global.h>
#include <CustomPass.h>
#include <Plugin.h>
#include <RenderTargets.h>
#include <ShaderResources.h>
#include <wincodec.h>

// Global custom resource to pass data to shaders.
REX::W32::ID3D11Buffer* g_customSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_customSRV = nullptr;
REX::W32::ID3D11Buffer* g_modularFloatsSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_modularFloatsSRV = nullptr;
REX::W32::ID3D11Buffer* g_modularIntsSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_modularIntsSRV = nullptr;
REX::W32::ID3D11Buffer* g_modularBoolsSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_modularBoolsSRV = nullptr;

// Global depth buffer SRV for shaders to read depth when DEPTHBUFFER_ON is enabled.
REX::W32::ID3D11ShaderResourceView* g_depthSRV = nullptr;

// Set while injected PS resources are mutating SRV slots to avoid re-entering
// the PSSetShaderResources hook.
thread_local bool g_bindingInjectedPixelResources = false;

namespace ShaderResources
{
    namespace
    {
        std::atomic<UINT> g_currentDepthTargetIndex{ DEPTHSTENCIL_TARGET_COUNT };
        std::atomic<UINT> g_currentRenderTargetCount{ 0 };
        std::atomic<bool> g_currentHasRenderTarget{ false };
        std::atomic<bool> g_waterReflectionCubeCaptureActive{ false };
        REX::W32::ID3D11ShaderResourceView* g_lastSceneDepthSRV = nullptr;

        bool g_activeReplacementPixelShader = false;
        bool g_activeReplacementPixelShaderUsesGFXInjected = false;
        bool g_activeReplacementPixelShaderUsesDrawTag = false;
        bool g_activeReplacementPixelShaderUsesModularFloats = false;
        bool g_activeReplacementPixelShaderUsesModularInts = false;
        bool g_activeReplacementPixelShaderUsesModularBools = false;
        bool g_activeReplacementPixelShaderNeedsResourceRebind = false;
        ShaderDefinition* g_activeReplacementPixelShaderDef = nullptr;

        thread_local std::uint32_t g_commandBufferReplayDepth = 0;

        struct alignas(16) ModularFloat4 {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            float w = 0.0f;
        };

        struct alignas(16) ModularInt4 {
            int32_t x = 0;
            int32_t y = 0;
            int32_t z = 0;
            int32_t w = 0;
        };

        struct alignas(16) ModularUInt4 {
            uint32_t x = 0;
            uint32_t y = 0;
            uint32_t z = 0;
            uint32_t w = 0;
        };

        std::vector<ModularFloat4> g_modularFloatData(1);
        std::vector<ModularInt4> g_modularIntData(1);
        std::vector<ModularUInt4> g_modularBoolData(1);
        UINT g_modularFloatElementCount = 0;
        UINT g_modularIntElementCount = 0;
        UINT g_modularBoolElementCount = 0;
        REX::W32::ID3D11Buffer* g_waterReflectionCubeMetaBuffer = nullptr;
        REX::W32::ID3D11ShaderResourceView* g_waterReflectionCubeMetaSRV = nullptr;
        std::vector<ModularFloat4> g_waterReflectionCubeMetaData(1);
        UINT g_waterReflectionCubeMetaElementCount = 0;
        bool g_waterReflectionCubeMetaReady = false;

        UINT PackedElementCount(std::size_t valueCount)
        {
            return static_cast<UINT>(((std::max)(valueCount, std::size_t{ 1 }) + 3) / 4);
        }

        template <class T>
        bool EnsureStructuredSRV(
            REX::W32::ID3D11Device* device,
            REX::W32::ID3D11Buffer*& buffer,
            REX::W32::ID3D11ShaderResourceView*& srv,
            UINT& currentElementCount,
            UINT requiredElementCount,
            const char* label)
        {
            requiredElementCount = (std::max)(requiredElementCount, 1u);
            if (buffer && srv && currentElementCount == requiredElementCount) {
                return true;
            }

            ReleaseSRVBuffer(buffer, srv);

            REX::W32::D3D11_BUFFER_DESC desc{};
            desc.usage = REX::W32::D3D11_USAGE_DYNAMIC;
            desc.byteWidth = sizeof(T) * requiredElementCount;
            desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            desc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
            desc.miscFlags = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.structureByteStride = sizeof(T);

            HRESULT hr = device->CreateBuffer(&desc, nullptr, &buffer);
            if (FAILED(hr)) {
                REX::WARN("EnsureStructuredSRV: Failed to create {} buffer. HRESULT: 0x{:08X}", label, hr);
                currentElementCount = 0;
                return false;
            }

            REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.format = REX::W32::DXGI_FORMAT_UNKNOWN;
            srvDesc.viewDimension = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.buffer.firstElement = 0;
            srvDesc.buffer.numElements = requiredElementCount;

            hr = device->CreateShaderResourceView(buffer, &srvDesc, &srv);
            if (FAILED(hr)) {
                REX::WARN("EnsureStructuredSRV: Failed to create {} SRV. HRESULT: 0x{:08X}", label, hr);
                ReleaseSRVBuffer(buffer, srv);
                currentElementCount = 0;
                return false;
            }

            currentElementCount = requiredElementCount;
            return true;
        }

        template <class T>
        bool UpdateStructuredSRV(REX::W32::ID3D11DeviceContext* context, REX::W32::ID3D11Buffer* buffer, const std::vector<T>& data)
        {
            if (!context || !buffer || data.empty()) {
                return false;
            }

            REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(buffer, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                memcpy(mapped.data, data.data(), sizeof(T) * data.size());
                context->Unmap(buffer, 0);
                return true;
            }
            return false;
        }

        REX::W32::ID3D11ShaderResourceView* GetMainDepthSRV()
        {
            if (!g_rendererData) {
                return nullptr;
            }

            return g_rendererData->depthStencilTargets[static_cast<UINT>(MAIN_DEPTHSTENCIL_TARGET)].srViewDepth;
        }

        bool DefinitionNeedsPixelResourceRebind(const ShaderDefinition* def) noexcept
        {
            return def &&
                   (def->usesGFXInjected ||
                    def->usesGFXDrawTag ||
                    def->usesGFXModularFloats ||
                    def->usesGFXModularInts ||
                    def->usesGFXModularBools ||
                    !def->replacementSRVs.empty() ||
                    !def->replacementTextures.empty() ||
                    CustomPass::g_registry.HasGlobalResourceBindings());
        }

        bool IsSupportedTextureExtension(const std::filesystem::path& path)
        {
            const auto ext = ToLower(path.extension().string());
            return ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp";
        }

        IWICImagingFactory* GetWICFactory()
        {
            static IWICImagingFactory* factory = nullptr;
            static std::once_flag initOnce;
            std::call_once(initOnce, [] {
                const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
                    REX::WARN("ReplacementTexture: CoInitializeEx failed (0x{:08X})", static_cast<unsigned>(coHr));
                }

                const HRESULT hr = CoCreateInstance(
                    CLSID_WICImagingFactory,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&factory));
                if (FAILED(hr)) {
                    REX::WARN("ReplacementTexture: failed to create WIC factory (0x{:08X})", static_cast<unsigned>(hr));
                    factory = nullptr;
                }
            });
            return factory;
        }

        bool CreateWICTextureSRV(REX::W32::ID3D11Device* device,
                                 const std::filesystem::path& file,
                                 REX::W32::ID3D11ShaderResourceView** outSRV)
        {
            if (!device || !outSRV) {
                return false;
            }
            *outSRV = nullptr;

            IWICImagingFactory* factory = GetWICFactory();
            if (!factory) {
                return false;
            }

            IWICBitmapDecoder* decoder = nullptr;
            HRESULT hr = factory->CreateDecoderFromFilename(
                file.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                &decoder);
            if (FAILED(hr)) {
                REX::WARN("ReplacementTexture: WIC failed to open {} (0x{:08X})", file.string(), static_cast<unsigned>(hr));
                return false;
            }

            IWICBitmapFrameDecode* frame = nullptr;
            hr = decoder->GetFrame(0, &frame);
            decoder->Release();
            if (FAILED(hr) || !frame) {
                REX::WARN("ReplacementTexture: WIC failed to read first frame from {} (0x{:08X})", file.string(), static_cast<unsigned>(hr));
                return false;
            }

            UINT width = 0;
            UINT height = 0;
            hr = frame->GetSize(&width, &height);
            if (FAILED(hr) || width == 0 || height == 0) {
                frame->Release();
                REX::WARN("ReplacementTexture: WIC invalid texture size for {} (0x{:08X})", file.string(), static_cast<unsigned>(hr));
                return false;
            }

            IWICFormatConverter* converter = nullptr;
            hr = factory->CreateFormatConverter(&converter);
            if (FAILED(hr) || !converter) {
                frame->Release();
                REX::WARN("ReplacementTexture: WIC failed to create format converter for {} (0x{:08X})", file.string(), static_cast<unsigned>(hr));
                return false;
            }

            hr = converter->Initialize(
                frame,
                GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
            frame->Release();
            if (FAILED(hr)) {
                converter->Release();
                REX::WARN("ReplacementTexture: WIC failed to convert {} to RGBA8 (0x{:08X})", file.string(), static_cast<unsigned>(hr));
                return false;
            }

            const UINT stride = width * 4;
            const UINT imageSize = stride * height;
            std::vector<std::uint8_t> pixels(imageSize);
            hr = converter->CopyPixels(nullptr, stride, imageSize, pixels.data());
            converter->Release();
            if (FAILED(hr)) {
                REX::WARN("ReplacementTexture: WIC failed to copy pixels from {} (0x{:08X})", file.string(), static_cast<unsigned>(hr));
                return false;
            }

            REX::W32::D3D11_TEXTURE2D_DESC desc{};
            desc.width = width;
            desc.height = height;
            desc.mipLevels = 1;
            desc.arraySize = 1;
            desc.format = REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.sampleDesc.count = 1;
            desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
            desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;

            REX::W32::D3D11_SUBRESOURCE_DATA initial{};
            initial.sysMem = pixels.data();
            initial.sysMemPitch = stride;

            REX::W32::ID3D11Texture2D* texture = nullptr;
            hr = device->CreateTexture2D(&desc, &initial, &texture);
            if (FAILED(hr) || !texture) {
                REX::WARN("ReplacementTexture: CreateTexture2D failed for {} (0x{:08X})", file.string(), static_cast<unsigned>(hr));
                return false;
            }

            hr = device->CreateShaderResourceView(texture, nullptr, outSRV);
            texture->Release();
            if (FAILED(hr) || !*outSRV) {
                REX::WARN("ReplacementTexture: CreateShaderResourceView failed for {} (0x{:08X})", file.string(), static_cast<unsigned>(hr));
                return false;
            }

            return true;
        }

        bool EnsureReplacementTextureSRV(REX::W32::ID3D11Device* device, ReplacementTextureBinding& binding)
        {
            if (!device || binding.slot < 0) {
                return false;
            }
            if (binding.srv) {
                return true;
            }
            if (binding.loadFailed) {
                return false;
            }

            binding.loadTried = true;
            if (!std::filesystem::exists(binding.file)) {
                REX::WARN("ReplacementTexture: file not found: {}", binding.file.string());
                binding.loadFailed = true;
                return false;
            }
            if (!IsSupportedTextureExtension(binding.file)) {
                REX::WARN("ReplacementTexture: unsupported texture extension: {}", binding.file.string());
                binding.loadFailed = true;
                return false;
            }

            if (!CreateWICTextureSRV(device, binding.file, &binding.srv)) {
                binding.loadFailed = true;
                return false;
            }
            REX::INFO("ReplacementTexture: loaded {} into t{}", binding.file.string(), binding.slot);
            return true;
        }

        std::string ReplacementSRVSourceName(const ReplacementSRVBinding& binding)
        {
            switch (binding.kind) {
            case ReplacementSRVSourceKind::Depth:
                return "depth";
            case ReplacementSRVSourceKind::SceneHDR:
                return "sceneHDR";
            case ReplacementSRVSourceKind::GBufferRT:
                return std::format("gbufferRT:{}", binding.gbufferIndex);
            case ReplacementSRVSourceKind::GBufferNormal:
                return "gbufferNormal";
            case ReplacementSRVSourceKind::GBufferAlbedo:
                return "gbufferAlbedo";
            case ReplacementSRVSourceKind::GBufferMaterial:
                return "gbufferMaterial";
            case ReplacementSRVSourceKind::MotionVectors:
                return "motionVectors";
            case ReplacementSRVSourceKind::WaterReflectionCubemap:
                return "waterReflectionCubemap";
            case ReplacementSRVSourceKind::WaterReflectionCubemapMeta:
                return "waterReflectionCubemapMeta";
            case ReplacementSRVSourceKind::CustomResource:
                return std::format("customResource:{}", binding.resourceName);
            default:
                return "(none)";
            }
        }

        REX::W32::ID3D11ShaderResourceView* ResolveWaterReflectionCubemap(
            REX::W32::ID3D11DeviceContext* context)
        {
            if (!context) {
                return nullptr;
            }

            auto publishMeta = [context](
                float valid, float mipCount, float cubeSize) {
                REX::W32::ID3D11Device* device = nullptr;
                context->GetDevice(&device);
                if (!device) {
                    return;
                }
                auto* previousMetaBuffer = g_waterReflectionCubeMetaBuffer;
                if (EnsureStructuredSRV<ModularFloat4>(
                        device,
                        g_waterReflectionCubeMetaBuffer,
                        g_waterReflectionCubeMetaSRV,
                        g_waterReflectionCubeMetaElementCount,
                        1u,
                        "water reflection cube metadata")) {
                    if (previousMetaBuffer != g_waterReflectionCubeMetaBuffer) {
                        g_waterReflectionCubeMetaReady = false;
                    }
                    static float previousValid = -1.0f;
                    static float previousMips = -1.0f;
                    static float previousSize = -1.0f;
                    static REX::W32::ID3D11Buffer* previousBuffer = nullptr;
                    if (valid != previousValid || mipCount != previousMips ||
                        cubeSize != previousSize ||
                        g_waterReflectionCubeMetaBuffer != previousBuffer) {
                        g_waterReflectionCubeMetaData[0] = {
                            valid, mipCount, cubeSize, 9137.25f };
                        if (UpdateStructuredSRV(
                            context,
                            g_waterReflectionCubeMetaBuffer,
                            g_waterReflectionCubeMetaData)) {
                            g_waterReflectionCubeMetaReady = true;
                            previousValid = valid;
                            previousMips = mipCount;
                            previousSize = cubeSize;
                            previousBuffer =
                                g_waterReflectionCubeMetaBuffer;
                        }
                    }
                }
                device->Release();
            };

            if (!g_rendererData) {
                publishMeta(0.0f, 0.0f, 0.0f);
                return nullptr;
            }

            auto& cube = g_rendererData->cubeMapRenderTargets[0];
            if (!cube.texture || !cube.srView) {
                publishMeta(0.0f, 0.0f, 0.0f);
                return nullptr;
            }

            // Directly publishing the engine cube is safe only while none of
            // its six faces is the active render target. OMGetRenderTargets
            // AddRefs every returned view, so release them before returning.
            REX::W32::ID3D11RenderTargetView* activeRTVs[8]{};
            context->OMGetRenderTargets(8, activeRTVs, nullptr);
            bool cubeFaceActive = g_waterReflectionCubeCaptureActive.load(
                std::memory_order_relaxed);
            for (auto* activeRTV : activeRTVs) {
                if (activeRTV) {
                    for (auto* cubeRTV : cube.rtView) {
                        cubeFaceActive = cubeFaceActive ||
                            (cubeRTV && activeRTV == cubeRTV);
                    }
                    if (!cubeFaceActive) {
                        REX::W32::ID3D11Resource* activeResource = nullptr;
                        activeRTV->GetResource(&activeResource);
                        cubeFaceActive = activeResource == cube.texture;
                        if (activeResource) {
                            activeResource->Release();
                        }
                    }
                    activeRTV->Release();
                }
            }

            static bool lastCubeFaceActive = false;
            if (cubeFaceActive != lastCubeFaceActive) {
                REX::INFO(
                    "WaterReflectionCube: capture {} - t49 {}",
                    cubeFaceActive ? "active" : "inactive",
                    cubeFaceActive ? "cleared" : "eligible");
                lastCubeFaceActive = cubeFaceActive;
            }
            if (cubeFaceActive) {
                publishMeta(0.0f, 0.0f, 0.0f);
                return nullptr;
            }

            REX::W32::D3D11_TEXTURE2D_DESC textureDesc{};
            REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            cube.texture->GetDesc(&textureDesc);
            cube.srView->GetDesc(&srvDesc);
            REX::W32::ID3D11Resource* srvResource = nullptr;
            cube.srView->GetResource(&srvResource);
            const bool srvMatchesTexture = srvResource == cube.texture;
            if (srvResource) {
                srvResource->Release();
            }
            const bool textureCube =
                (textureDesc.miscFlags &
                 REX::W32::D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
            const UINT mostDetailedMip = srvDesc.textureCube.mostDetailedMip;
            const UINT declaredVisibleMips = srvDesc.textureCube.mipLevels;
            const UINT availableMips = mostDetailedMip < textureDesc.mipLevels
                ? textureDesc.mipLevels - mostDetailedMip
                : 0u;
            const UINT visibleMips = declaredVisibleMips == UINT_MAX
                ? availableMips
                : (std::min)(declaredVisibleMips, availableMips);
            const UINT visibleBaseSize = availableMips > 0u
                ? (std::max)(textureDesc.width >> mostDetailedMip, 1u)
                : 0u;
            const bool valid = srvMatchesTexture &&
                textureDesc.arraySize == 6u && textureCube &&
                srvDesc.viewDimension ==
                    REX::W32::D3D11_SRV_DIMENSION_TEXTURECUBE &&
                visibleMips > 0u && visibleBaseSize > 0u;

            static REX::W32::ID3D11Texture2D* loggedTexture = nullptr;
            if (cube.texture != loggedTexture) {
                REX::INFO(
                    "WaterReflectionCube: texture=0x{:016X} srv=0x{:016X} {}x{} array={} resourceMips={} srvMostDetailed={} srvMips={} usableMips=1 format={} srvDim={} valid={}",
                    reinterpret_cast<std::uintptr_t>(cube.texture),
                    reinterpret_cast<std::uintptr_t>(cube.srView),
                    textureDesc.width, textureDesc.height,
                    textureDesc.arraySize, textureDesc.mipLevels,
                    mostDetailedMip, visibleMips,
                    static_cast<unsigned>(textureDesc.format),
                    static_cast<unsigned>(srvDesc.viewDimension), valid);
                loggedTexture = cube.texture;
            }
            publishMeta(
                valid ? 1.0f : 0.0f,
                // Descriptor allocation does not prove that FO4 populated or
                // generated lower mips. Publish LOD0 only until runtime
                // telemetry establishes a complete roughness mip chain.
                valid ? 1.0f : 0.0f,
                valid ? static_cast<float>(visibleBaseSize) : 0.0f);
            return valid ? cube.srView : nullptr;
        }

        REX::W32::ID3D11ShaderResourceView* ResolveReplacementSRV(
            REX::W32::ID3D11DeviceContext* context,
            ReplacementSRVBinding& binding)
        {
            if (!g_rendererData) {
                return nullptr;
            }

            switch (binding.kind) {
            case ReplacementSRVSourceKind::Depth:
                g_depthSRV = GetDepthBufferSRV_Internal();
                return g_depthSRV;
            case ReplacementSRVSourceKind::SceneHDR:
                return g_rendererData->renderTargets[RT::idx(RT::Color::kMain)].srView;
            case ReplacementSRVSourceKind::GBufferRT:
                if (binding.gbufferIndex >= 0 && binding.gbufferIndex < 101) {
                    return g_rendererData->renderTargets[binding.gbufferIndex].srView;
                }
                return nullptr;
            case ReplacementSRVSourceKind::GBufferNormal:
                if (NORMAL_BUFFER_INDEX >= 0 && NORMAL_BUFFER_INDEX < 101) {
                    return g_rendererData->renderTargets[NORMAL_BUFFER_INDEX].srView;
                }
                return nullptr;
            case ReplacementSRVSourceKind::GBufferAlbedo:
                return g_rendererData->renderTargets[RT::idx(RT::Color::kGbufferAlbedo)].srView;
            case ReplacementSRVSourceKind::GBufferMaterial:
                return g_rendererData->renderTargets[RT::idx(RT::Color::kGbufferMaterial)].srView;
            case ReplacementSRVSourceKind::MotionVectors:
                return g_rendererData->renderTargets[RT::idx(RT::Color::kMotionVectors)].srView;
            case ReplacementSRVSourceKind::WaterReflectionCubemap:
                return ResolveWaterReflectionCubemap(context);
            case ReplacementSRVSourceKind::WaterReflectionCubemapMeta:
                ResolveWaterReflectionCubemap(context);
                return g_waterReflectionCubeMetaReady
                    ? g_waterReflectionCubeMetaSRV
                    : nullptr;
            case ReplacementSRVSourceKind::CustomResource:
                return CustomPass::g_registry.GetResourceSRV(binding.resourceName);
            default:
                return nullptr;
            }
        }

        void BindCustomShaderResources(REX::W32::ID3D11DeviceContext* context,
                                       bool pixelShader,
                                       bool bindGFXInjected,
                                       bool bindDrawTag,
                                       bool bindModularFloats,
                                       bool bindModularInts,
                                       bool bindModularBools,
                                       REX::W32::ID3D11ShaderResourceView* drawTagOverride = nullptr)
        {
            if (!context) {
                return;
            }

            if (bindGFXInjected && g_customSRV) {
                if (pixelShader) {
                    context->PSSetShaderResources(CUSTOMBUFFER_SLOT, 1, &g_customSRV);
                } else {
                    context->VSSetShaderResources(CUSTOMBUFFER_SLOT, 1, &g_customSRV);
                }
            }

            auto* drawTagSRV = drawTagOverride ? drawTagOverride : g_drawTagSRV;
            if (bindDrawTag && drawTagSRV && g_commandBufferReplayDepth == 0) {
                if (pixelShader) {
                    context->PSSetShaderResources(DRAWTAG_SLOT, 1, &drawTagSRV);
                } else {
                    context->VSSetShaderResources(DRAWTAG_SLOT, 1, &drawTagSRV);
                }
            }

            if (bindModularFloats && g_modularFloatsSRV) {
                if (pixelShader) {
                    context->PSSetShaderResources(MODULAR_FLOATS_SLOT, 1, &g_modularFloatsSRV);
                } else {
                    context->VSSetShaderResources(MODULAR_FLOATS_SLOT, 1, &g_modularFloatsSRV);
                }
            }
            if (bindModularInts && g_modularIntsSRV) {
                if (pixelShader) {
                    context->PSSetShaderResources(MODULAR_INTS_SLOT, 1, &g_modularIntsSRV);
                } else {
                    context->VSSetShaderResources(MODULAR_INTS_SLOT, 1, &g_modularIntsSRV);
                }
            }
            if (bindModularBools && g_modularBoolsSRV) {
                if (pixelShader) {
                    context->PSSetShaderResources(MODULAR_BOOLS_SLOT, 1, &g_modularBoolsSRV);
                } else {
                    context->VSSetShaderResources(MODULAR_BOOLS_SLOT, 1, &g_modularBoolsSRV);
                }
            }
        }
    }

    void ReleaseSRVBuffer(REX::W32::ID3D11Buffer*& buffer, REX::W32::ID3D11ShaderResourceView*& srv)
    {
        if (srv) {
            srv->Release();
            srv = nullptr;
        }
        if (buffer) {
            buffer->Release();
            buffer = nullptr;
        }
    }

    void PackModularShaderValues(GlobalShaderSettings& settings)
    {
        g_modularFloatData.assign(PackedElementCount(settings.GetFloatShaderValues().size()), {});
        g_modularIntData.assign(PackedElementCount(settings.GetIntShaderValues().size()), {});
        g_modularBoolData.assign(PackedElementCount(settings.GetBoolShaderValues().size()), {});

        auto setFloatComponent = [](ModularFloat4& item, uint32_t component, float value) {
            switch (component) {
            case 0: item.x = value; break;
            case 1: item.y = value; break;
            case 2: item.z = value; break;
            case 3: item.w = value; break;
            }
        };
        auto setIntComponent = [](ModularInt4& item, uint32_t component, int32_t value) {
            switch (component) {
            case 0: item.x = value; break;
            case 1: item.y = value; break;
            case 2: item.z = value; break;
            case 3: item.w = value; break;
            }
        };
        auto setUIntComponent = [](ModularUInt4& item, uint32_t component, uint32_t value) {
            switch (component) {
            case 0: item.x = value; break;
            case 1: item.y = value; break;
            case 2: item.z = value; break;
            case 3: item.w = value; break;
            }
        };

        for (auto* sv : settings.GetFloatShaderValues()) {
            if (!sv) {
                continue;
            }
            const uint32_t element = sv->bufferIndex / 4;
            if (element < g_modularFloatData.size()) {
                setFloatComponent(g_modularFloatData[element], sv->bufferIndex % 4, sv->current.f);
            }
        }
        for (auto* sv : settings.GetIntShaderValues()) {
            if (!sv) {
                continue;
            }
            const uint32_t element = sv->bufferIndex / 4;
            if (element < g_modularIntData.size()) {
                setIntComponent(g_modularIntData[element], sv->bufferIndex % 4, sv->current.i);
            }
        }
        for (auto* sv : settings.GetBoolShaderValues()) {
            if (!sv) {
                continue;
            }
            const uint32_t element = sv->bufferIndex / 4;
            if (element < g_modularBoolData.size()) {
                setUIntComponent(g_modularBoolData[element], sv->bufferIndex % 4, sv->current.b ? 1u : 0u);
            }
        }
    }

    void EnsureInjectedShaderResourceViews(REX::W32::ID3D11Device* device)
    {
        if (!device) {
            return;
        }

        if (!g_customSRVBuffer) {
            REX::W32::D3D11_BUFFER_DESC desc{};
            desc.usage = REX::W32::D3D11_USAGE_DYNAMIC;
            desc.byteWidth = sizeof(GFXBoosterAccessData);
            desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
            desc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
            desc.miscFlags = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            desc.structureByteStride = sizeof(GFXBoosterAccessData);
            HRESULT hr = device->CreateBuffer(&desc, nullptr, &g_customSRVBuffer);
            if (FAILED(hr)) {
                REX::WARN("UpdateCustomBuffer_Internal: Failed to create custom buffer. HRESULT: 0x{:08X}", hr);
                return;
            }
        }

        if (g_customSRVBuffer && !g_customSRV) {
            REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.format = REX::W32::DXGI_FORMAT_UNKNOWN;
            srvDesc.viewDimension = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
            srvDesc.buffer.firstElement = 0;
            srvDesc.buffer.numElements = 1;
            HRESULT hr = device->CreateShaderResourceView(g_customSRVBuffer, &srvDesc, &g_customSRV);
            if (FAILED(hr)) {
                REX::WARN("UpdateCustomBuffer_Internal: Failed to create custom SRV. HRESULT: 0x{:08X}", hr);
                return;
            }
        }

        EnsureStructuredSRV<ModularFloat4>(
            device,
            g_modularFloatsSRVBuffer,
            g_modularFloatsSRV,
            g_modularFloatElementCount,
            static_cast<UINT>(g_modularFloatData.size()),
            "modular floats");
        EnsureStructuredSRV<ModularInt4>(
            device,
            g_modularIntsSRVBuffer,
            g_modularIntsSRV,
            g_modularIntElementCount,
            static_cast<UINT>(g_modularIntData.size()),
            "modular ints");
        EnsureStructuredSRV<ModularUInt4>(
            device,
            g_modularBoolsSRVBuffer,
            g_modularBoolsSRV,
            g_modularBoolElementCount,
            static_cast<UINT>(g_modularBoolData.size()),
            "modular bools");
    }

    void UpdateInjectedShaderResourceViews(REX::W32::ID3D11DeviceContext* context)
    {
        if (g_customSRVBuffer && context) {
            REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(g_customSRVBuffer, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                memcpy(mapped.data, &g_customBufferData, sizeof(g_customBufferData));
                context->Unmap(g_customSRVBuffer, 0);
            }
        }

        UpdateStructuredSRV(context, g_modularFloatsSRVBuffer, g_modularFloatData);
        UpdateStructuredSRV(context, g_modularIntsSRVBuffer, g_modularIntData);
        UpdateStructuredSRV(context, g_modularBoolsSRVBuffer, g_modularBoolData);
    }

    void BindInjectedPixelShaderResources(REX::W32::ID3D11DeviceContext* context)
    {
        if (!context || g_customPassRendering || !g_activeReplacementPixelShaderNeedsResourceRebind) {
            return;
        }

        g_bindingInjectedPixelResources = true;
        BindCustomShaderResources(
            context,
            true,
            g_activeReplacementPixelShaderUsesGFXInjected,
            g_activeReplacementPixelShaderUsesDrawTag,
            g_activeReplacementPixelShaderUsesModularFloats,
            g_activeReplacementPixelShaderUsesModularInts,
            g_activeReplacementPixelShaderUsesModularBools);

        if (g_activeReplacementPixelShaderUsesGFXInjected) {
            g_depthSRV = GetDepthBufferSRV_Internal();
            if (g_depthSRV) {
                context->PSSetShaderResources(DEPTHBUFFER_SLOT, 1, &g_depthSRV);
            } else if (DEBUGGING) {
                REX::WARN("BindInjectedPixelShaderResources: No depth SRV available for t{}", DEPTHBUFFER_SLOT);
            }
        }

        CustomPass::g_registry.BindGlobalResourceSRVs(context, /*pixelStage=*/true);
        BindReplacementSRVResources(context, g_activeReplacementPixelShaderDef, /*pixelStage=*/true);
        BindReplacementTextureResources(context, g_activeReplacementPixelShaderDef, /*pixelStage=*/true);

        g_bindingInjectedPixelResources = false;
    }

    void BindInjectedVertexShaderResources(REX::W32::ID3D11DeviceContext* context)
    {
        if (!context) {
            return;
        }

        BindCustomShaderResources(context, false, true, false, true, true, true);
        CustomPass::g_registry.BindGlobalResourceSRVs(context, /*pixelStage=*/false);
    }

    void BindReplacementTextureResources(
        REX::W32::ID3D11DeviceContext* context,
        ShaderDefinition* def,
        bool pixelStage)
    {
        if (!context || !def || def->replacementTextures.empty()) {
            return;
        }

        REX::W32::ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (!device) {
            return;
        }

        for (auto& binding : def->replacementTextures) {
            if (binding.slot < 0 || !EnsureReplacementTextureSRV(device, binding)) {
                continue;
            }
            if (pixelStage) {
                context->PSSetShaderResources(static_cast<UINT>(binding.slot), 1, &binding.srv);
            } else {
                context->VSSetShaderResources(static_cast<UINT>(binding.slot), 1, &binding.srv);
            }
        }

        device->Release();
    }

    void BindReplacementSRVResources(
        REX::W32::ID3D11DeviceContext* context,
        ShaderDefinition* def,
        bool pixelStage)
    {
        if (!context || !def || def->replacementSRVs.empty()) {
            return;
        }

        for (auto& binding : def->replacementSRVs) {
            if (binding.slot < 0 || binding.slot >= 128) {
                continue;
            }

            REX::W32::ID3D11ShaderResourceView* srv =
                ResolveReplacementSRV(context, binding);
            if (!srv) {
                // This source is intentionally rebound even when unavailable:
                // clearing t49 prevents inherited/stale SRVs and is also the
                // required RTV/SRV hazard break during cube-face capture.
                if (binding.kind ==
                        ReplacementSRVSourceKind::WaterReflectionCubemap ||
                    binding.kind ==
                        ReplacementSRVSourceKind::WaterReflectionCubemapMeta) {
                    if (pixelStage) {
                        context->PSSetShaderResources(
                            static_cast<UINT>(binding.slot), 1, &srv);
                    } else {
                        context->VSSetShaderResources(
                            static_cast<UINT>(binding.slot), 1, &srv);
                    }
                    continue;
                }
                if (!binding.warnedMissing) {
                    REX::WARN("ReplacementSRV[{}]: source '{}' unavailable for t{}",
                        def->id, ReplacementSRVSourceName(binding), binding.slot);
                    binding.warnedMissing = true;
                }
                continue;
            }

            if (pixelStage) {
                context->PSSetShaderResources(static_cast<UINT>(binding.slot), 1, &srv);
            } else {
                context->VSSetShaderResources(static_cast<UINT>(binding.slot), 1, &srv);
            }
        }
    }

    // Public face of the lazy WIC texture loader so custom passes can bind
    // file-backed textures (`input=N:file:foo.png`) through the exact same
    // load/cache/fail-once path replacement shaders use for bindTexture.
    bool EnsureFileTextureSRV(REX::W32::ID3D11Device* device, ReplacementTextureBinding& binding)
    {
        return EnsureReplacementTextureSRV(device, binding);
    }

    REX::W32::ID3D11ShaderResourceView* GetDepthBufferSRV_Internal()
    {
        if (!g_rendererData) {
            REX::WARN("GetDepthBufferSRV_Internal: RendererData not available.");
            return nullptr;
        }

        if (auto* mainDepthSRV = GetMainDepthSRV()) {
            g_lastSceneDepthSRV = mainDepthSRV;
            return mainDepthSRV;
        }

        if (g_rendererData->context) {
            REX::W32::ID3D11DepthStencilView* currentDSV = nullptr;
            g_rendererData->context->OMGetRenderTargets(0, nullptr, &currentDSV);
            if (currentDSV) {
                if (FindDepthTargetIndexForDSV(currentDSV) == static_cast<UINT>(MAIN_DEPTHSTENCIL_TARGET)) {
                    g_lastSceneDepthSRV = GetMainDepthSRV();
                    currentDSV->Release();
                    return g_lastSceneDepthSRV;
                }
                currentDSV->Release();
            }
        }

        if (g_lastSceneDepthSRV && g_lastSceneDepthSRV == GetMainDepthSRV()) {
            return g_lastSceneDepthSRV;
        }

        return nullptr;
    }

    UINT FindDepthTargetIndexForDSV(REX::W32::ID3D11DepthStencilView* dsv)
    {
        if (!g_rendererData || !dsv) {
            return DEPTHSTENCIL_TARGET_COUNT;
        }

        for (UINT i = 0; i < DEPTHSTENCIL_TARGET_COUNT; ++i) {
            auto& target = g_rendererData->depthStencilTargets[i];
            for (int viewIndex = 0; viewIndex < 4; ++viewIndex) {
                if (target.dsView[viewIndex] == dsv ||
                    target.dsViewReadOnlyDepth[viewIndex] == dsv ||
                    target.dsViewReadOnlyStencil[viewIndex] == dsv ||
                    target.dsViewReadOnlyDepthStencil[viewIndex] == dsv) {
                    return i;
                }
            }
        }

        return DEPTHSTENCIL_TARGET_COUNT;
    }

    void PrepareWaterReflectionCubeOM(
        REX::W32::ID3D11DeviceContext* context,
        UINT numViews,
        REX::W32::ID3D11RenderTargetView* const* renderTargetViews)
    {
        bool cubeFaceBound = false;
        bool mainViewBound = false;
        int activeFace = -1;
        if (g_rendererData && renderTargetViews) {
            auto& cube = g_rendererData->cubeMapRenderTargets[0];
            auto& mainTarget = g_rendererData->renderTargets[
                RT::idx(RT::Color::kMain)];
            for (UINT i = 0; i < numViews; ++i) {
                auto* incomingRTV = renderTargetViews[i];
                if (!incomingRTV) {
                    continue;
                }
                for (int face = 0; face < 6; ++face) {
                    if (cube.rtView[face] && incomingRTV == cube.rtView[face]) {
                        cubeFaceBound = true;
                        activeFace = face;
                        break;
                    }
                }
                REX::W32::ID3D11Resource* incomingResource = nullptr;
                incomingRTV->GetResource(&incomingResource);
                cubeFaceBound = cubeFaceBound ||
                    (cube.texture && incomingResource == cube.texture);
                mainViewBound = mainViewBound ||
                    (mainTarget.texture &&
                     incomingResource == mainTarget.texture);
                if (incomingResource) {
                    incomingResource->Release();
                }
            }
        }

        const bool wasActive = g_waterReflectionCubeCaptureActive.load(
            std::memory_order_relaxed);
        // Keep the scope latched through intermediate targets used by the cube
        // camera. A main HDR bind (or Present) is the explicit exit boundary.
        const bool captureActive = cubeFaceBound ||
            (wasActive && !mainViewBound);
        g_waterReflectionCubeCaptureActive.store(
            captureActive, std::memory_order_relaxed);
        if (captureActive && context) {
            // Clear before the RTV bind reaches D3D11. The guard prevents the
            // PS-SRV hook from reasserting replacement resources behind us.
            REX::W32::ID3D11ShaderResourceView* nullSRV = nullptr;
            g_bindingInjectedPixelResources = true;
            context->PSSetShaderResources(49u, 1u, &nullSRV);
            context->PSSetShaderResources(51u, 1u, &nullSRV);
            g_bindingInjectedPixelResources = false;
        }
        if (captureActive != wasActive) {
            if (captureActive) {
                REX::INFO(
                    "WaterReflectionCube: entering face capture face={} - t49/t51 cleared before OM bind",
                    activeFace);
            } else {
                REX::INFO(
                    "WaterReflectionCube: leaving face capture - main-view publication eligible");
            }
        }
    }

    bool WaterReflectionCubeCaptureActive() noexcept
    {
        return g_waterReflectionCubeCaptureActive.load(
            std::memory_order_relaxed);
    }

    void EndWaterReflectionCubeFrame() noexcept
    {
        g_waterReflectionCubeCaptureActive.store(
            false, std::memory_order_relaxed);
    }

    void Shutdown()
    {
        ReleaseSRVBuffer(
            g_waterReflectionCubeMetaBuffer,
            g_waterReflectionCubeMetaSRV);
        g_waterReflectionCubeMetaElementCount = 0;
        g_waterReflectionCubeMetaReady = false;
        EndWaterReflectionCubeFrame();
    }

    void TrackOMRenderTargets(
        UINT numViews,
        REX::W32::ID3D11RenderTargetView* const* renderTargetViews,
        REX::W32::ID3D11DepthStencilView* depthStencilView)
    {
        bool hasRT = false;
        if (renderTargetViews) {
            for (UINT i = 0; i < numViews; ++i) {
                if (renderTargetViews[i]) {
                    hasRT = true;
                    break;
                }
            }
        }

        const UINT depthTarget = FindDepthTargetIndexForDSV(depthStencilView);
        g_currentDepthTargetIndex.store(depthTarget, std::memory_order_relaxed);
        g_currentRenderTargetCount.store(numViews, std::memory_order_relaxed);
        g_currentHasRenderTarget.store(hasRT, std::memory_order_relaxed);

        if (depthTarget == static_cast<UINT>(MAIN_DEPTHSTENCIL_TARGET)) {
            g_lastSceneDepthSRV = GetMainDepthSRV();
        }
    }

    UINT GetCurrentDepthTargetIndex() noexcept
    {
        return g_currentDepthTargetIndex.load(std::memory_order_relaxed);
    }

    bool HasCurrentRenderTarget() noexcept
    {
        return g_currentHasRenderTarget.load(std::memory_order_relaxed);
    }

    bool ActiveReplacementPixelShaderActive() noexcept
    {
        return g_activeReplacementPixelShader;
    }

    bool ActiveReplacementPixelShaderUsesDrawTag() noexcept
    {
        return g_activeReplacementPixelShaderUsesDrawTag;
    }

    bool ActiveReplacementPixelShaderNeedsResourceRebind() noexcept
    {
        return g_activeReplacementPixelShaderNeedsResourceRebind;
    }

    void SetActiveReplacementPixelShaderUsage(const ShaderDefinition* def, bool active) noexcept
    {
        g_activeReplacementPixelShader = active;
        g_activeReplacementPixelShaderDef = active ? const_cast<ShaderDefinition*>(def) : nullptr;
        g_activeReplacementPixelShaderUsesGFXInjected = active && def && def->usesGFXInjected;
        g_activeReplacementPixelShaderUsesDrawTag = active && def && def->usesGFXDrawTag;
        g_activeReplacementPixelShaderUsesModularFloats = active && def && def->usesGFXModularFloats;
        g_activeReplacementPixelShaderUsesModularInts = active && def && def->usesGFXModularInts;
        g_activeReplacementPixelShaderUsesModularBools = active && def && def->usesGFXModularBools;
        g_activeReplacementPixelShaderNeedsResourceRebind = active && DefinitionNeedsPixelResourceRebind(def);
    }

    bool IsCommandBufferReplayActive() noexcept
    {
        return g_commandBufferReplayDepth != 0;
    }

    void EnterCommandBufferReplay() noexcept
    {
        ++g_commandBufferReplayDepth;
    }

    void LeaveCommandBufferReplay() noexcept
    {
        --g_commandBufferReplayDepth;
    }
}
