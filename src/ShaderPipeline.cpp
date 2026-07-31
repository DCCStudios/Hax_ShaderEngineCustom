#include <Global.h>
#include <PCH.h>
#include <CustomPass.h>
#include <ShaderPipeline.h>

std::atomic_bool g_anyReplacementShaderUsesDrawTag{ false };

// A definition may have been precompiled before the engine creates the
// matching vanilla shader. Seed that new entry from the already-created
// replacement while holding the definition's compile lock. Compile completion
// handles the opposite ordering by publishing to entries already in ShaderDB.
static void AttachCompiledReplacementToEntry(
    ShaderDBEntry& entry,
    ShaderDefinition* def)
{
    if (!def) return;
    if (!def->compileMutex) def->compileMutex = std::make_unique<std::mutex>();
    std::lock_guard compileLock(*def->compileMutex);
    if (def->type == ShaderType::Pixel) {
        entry.SetReplacementPixelShader(def->loadedPixelShader);
    } else {
        entry.SetReplacementVertexShader(def->loadedVertexShader);
    }
}

// Analyze the shader bytecode to extract info for matching and potential replacement.
ShaderDBEntry AnalyzeShader_Internal(REX::W32::ID3D11PixelShader* pixelShader, REX::W32::ID3D11VertexShader* vertexShader, std::vector<uint8_t> bytecode, SIZE_T BytecodeLength) {
    ShaderDBEntry entry{};
    if (!pixelShader && !vertexShader || bytecode.empty()) return entry;
    void* shader = nullptr;
    if (pixelShader) {
        shader = static_cast<void*>(pixelShader);
    } else if (vertexShader) {
        shader = static_cast<void*>(vertexShader);
    }
    entry.originalShader = shader;
    if (pixelShader) {
        entry.type = ShaderType::Pixel;
    } else if (vertexShader) {
        entry.type = ShaderType::Vertex;
    }
    entry.bytecode = std::move(bytecode);
    auto hash = static_cast<std::uint32_t>(std::hash<std::string_view>{}(std::string_view((char*)entry.bytecode.data(), entry.bytecode.size())));
    entry.hash = hash;
    entry.size = BytecodeLength;
    // Analyze the shader entry
    if (ReflectShader_Internal(entry)) {
        if (DEBUGGING) {
            REX::INFO("AnalyzeShader_Internal: Shader reflection of {} Shader successful. Hash={:08X}, Size={}", entry.type == ShaderType::Vertex ? "Vertex" : "Pixel", hash, entry.size);
        }
    } else {
        REX::WARN("AnalyzeShader_Internal: Shader reflection failed for {} Shader with hash {:08X} and size {} bytes.", entry.type == ShaderType::Vertex ? "Vertex" : "Pixel", hash, entry.size);
    }
    // Validate entry before returning
    if (!entry.hash || !entry.size) {
        entry.SetValid(false);
        return entry; // Invalid entry, do not add to ShaderDB
    } else {
        entry.SetValid(true);
    }
    // Compare to shader definitions in our INI and find a match based on filters
    // If we find a match, we can store the compiled replacement shader in the entry for quick access during rendering.
    std::shared_lock lock(g_shaderDefinitions.mutex);
    for (ShaderDefinition* def : g_shaderDefinitions.definitions) {
        if (def->active && DoesEntryMatchDefinition_Internal(entry, def)) {
            entry.SetMatched(true);
            entry.matchedDefinition = def; // Store the matched definition for later use during shader compilation
            AttachCompiledReplacementToEntry(entry, def);
            if (DEVELOPMENT && def->log) {
                REX::INFO("AnalyzeShader_Internal: ------------------------------------------------");
                    REX::INFO("RematchAllShaders_Internal: Found matching shader definition '{}' for {} shader with ShaderUID '{}'.", def->id, entry.type == ShaderType::Vertex ? "Vertex" : "Pixel", entry.shaderUID);
                    REX::INFO("RematchAllShaders_Internal: Shader Hash: {:08X}", entry.hash);
                    REX::INFO("RematchAllShaders_Internal: Shader Size: {} bytes", entry.size);
                    REX::INFO("RematchAllShaders_Internal: Shader ASM Hash: {:08X}", entry.asmHash);
                REX::INFO(" - Shader CB Sizes: {},{},{},{},{},{},{},{},{},{},{},{},{},{}", 
                    entry.expectedCBSizes[0], entry.expectedCBSizes[1], entry.expectedCBSizes[2], entry.expectedCBSizes[3],
                    entry.expectedCBSizes[4], entry.expectedCBSizes[5], entry.expectedCBSizes[6], entry.expectedCBSizes[7],
                    entry.expectedCBSizes[8], entry.expectedCBSizes[9], entry.expectedCBSizes[10], entry.expectedCBSizes[11],
                    entry.expectedCBSizes[12], entry.expectedCBSizes[13]);
                REX::INFO(" - Shader Texture Register Slots: {}", entry.textureSlots.empty() ? "None" : "");
                for (const auto& slot : entry.textureSlots) {
                    REX::INFO("   - Slot: t{}", slot);
                }
                REX::INFO(" - Shader Texture Dimensions: {}", entry.textureDimensions.empty() ? "None" : "");
                for (const auto& [dimension, slot] : entry.textureDimensions) {
                    REX::INFO("   - Dimension: {}, Slot: t{}", dimension, slot);
                }
                REX::INFO(" - Shader Texture Usage Bitmask: 0x{:08X}", entry.textureSlotMask);
                REX::INFO(" - Shader Texture Dimension Bitmask: 0x{:08X}", entry.textureDimensionMask);
                REX::INFO(" - Shader Input Texture Count: {}", entry.inputTextureCount != -1 ? std::to_string(entry.inputTextureCount) : "X");
                REX::INFO(" - Shader Input Count: {}", entry.inputCount != -1 ? std::to_string(entry.inputCount) : "X");
                REX::INFO(" - Shader Input Mask: 0x{:08X}", entry.inputMask);
                REX::INFO(" - Shader Output Count: {}", entry.outputCount != -1 ? std::to_string(entry.outputCount) : "X");
                REX::INFO(" - Shader Output Mask: 0x{:08X}", entry.outputMask);
                REX::INFO("AnalyzeShader_Internal: ------------------------------------------------");
            }
            if (DEVELOPMENT && def->dump && !entry.IsDumped()) {
                DumpOriginalShader_Internal(entry, def);
                entry.SetDumped(true);
            }
            break; // Stop checking after the first match to keep priorities based on order in INI
        }
    }
    return entry;
}

void UpdateReplacementResourceUsageFromReflection(ShaderDefinition* def)
{
    if (!def || !def->compiledShader) {
        return;
    }

    def->usesGFXInjected = false;
    def->usesGFXDrawTag = false;
    def->usesGFXModularFloats = false;
    def->usesGFXModularInts = false;
    def->usesGFXModularBools = false;

    ID3D11ShaderReflection* reflection = nullptr;
    const HRESULT hr = D3DReflect(
        def->compiledShader->GetBufferPointer(),
        def->compiledShader->GetBufferSize(),
        IID_ID3D11ShaderReflection,
        reinterpret_cast<void**>(&reflection));
    if (FAILED(hr) || !reflection) {
        REX::WARN("CompileShader_Internal: D3DReflect failed for '{}' (0x{:08X}); conservatively binding injected resources",
                  def->id,
                  static_cast<unsigned>(hr));
        def->usesGFXInjected = true;
        def->usesGFXDrawTag = true;
        def->usesGFXModularFloats = true;
        def->usesGFXModularInts = true;
        def->usesGFXModularBools = true;
        g_anyReplacementShaderUsesDrawTag.store(true, std::memory_order_release);
        return;
    }

    D3D11_SHADER_DESC shaderDesc{};
    if (SUCCEEDED(reflection->GetDesc(&shaderDesc))) {
        for (UINT i = 0; i < shaderDesc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bindDesc{};
            if (FAILED(reflection->GetResourceBindingDesc(i, &bindDesc)) || !bindDesc.Name) {
                continue;
            }

            const std::string_view name(bindDesc.Name);
            if (name == "GFXInjected") {
                def->usesGFXInjected = true;
            } else if (name == "GFXDrawTag") {
                def->usesGFXDrawTag = true;
            } else if (name == "GFXModularFloats") {
                def->usesGFXModularFloats = true;
            } else if (name == "GFXModularInts") {
                def->usesGFXModularInts = true;
            } else if (name == "GFXModularBools") {
                def->usesGFXModularBools = true;
            }
        }
    }
    reflection->Release();

    if (def->usesGFXDrawTag) {
        g_anyReplacementShaderUsesDrawTag.store(true, std::memory_order_release);
    }
}

// Compile the HLSL shaders that were defined in the INI for each shader
bool CompileShader_Internal(ShaderDefinition* def) {
    if (!def) return false;
    // Lazy-init in case a def slipped through without one (defensive).
    if (!def->compileMutex) def->compileMutex = std::make_unique<std::mutex>();
    // Per-def serialization: if the precompile worker is mid-flight on this
    // def the render thread blocks here briefly, then sees loadedPixelShader
    // already populated and short-circuits. Same in reverse. The lock spans
    // the cache lookup + D3DCompile + Create*Shader so observers always see
    // a consistent (compiledShader, loadedPixelShader/VertexShader) pair.
    std::lock_guard compileLock(*def->compileMutex);
    // Check if already compiled (re-checked under the lock so a concurrent
    // compile that finished while we were waiting is observed correctly).
    if (def->loadedPixelShader || def->loadedVertexShader) {
        // Repair entries matched after an earlier precompile. This is cheap in
        // the normal path and keeps the Shader Monitor tied to actual object
        // readiness instead of whether the permutation has executed yet.
        g_ShaderDB.PublishReplacementForDefinition(def);
        if (DEBUGGING)
            REX::INFO("CompileShader_Internal: Shader '{}' is already compiled. Skipping compilation.", def->id);
        return true;
    }
    // Check the file exists
    std::ifstream shaderFile(def->shaderFile, std::ios::binary);
    if (!shaderFile.good()) {
        REX::WARN("CompileShader_Internal: Shader file not found: {}", def->shaderFile.string());
        return false;
    }
    // Build the common shader header with dynamic shader settings values injected as defines
    std::string shaderHeader = GetCommonShaderHeaderHLSLTop();
    // Struct is already closed in commonShaderHeaderHLSLTop.
    // Add the bottom (declares GFXInjected) before the #defines that reference it.
    shaderHeader += GetCommonShaderHeaderHLSLBottom();
    // Inject #define named accessors; each maps a friendly name to the correct array slot.
    // bufferIndex is packed into float4/int4/uint4 settings buffers.
    shaderHeader += "// shader settings named accessors\n";
    for (auto* sValue : g_shaderSettings.GetFloatShaderValues()) {
        shaderHeader += std::format("#define {} GFXModularFloats[{}]{}\n", sValue->id, sValue->bufferIndex / 4, std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sValue->bufferIndex % 4]);
    }
    for (auto* sValue : g_shaderSettings.GetIntShaderValues()) {
        shaderHeader += std::format("#define {} GFXModularInts[{}]{}\n", sValue->id, sValue->bufferIndex / 4, std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sValue->bufferIndex % 4]);
    }
    for (auto* sValue : g_shaderSettings.GetBoolShaderValues()) {
        shaderHeader += std::format("#define {} (GFXModularBools[{}]{} != 0)\n", sValue->id, sValue->bufferIndex / 4, std::array<std::string_view, 4>{ ".x", ".y", ".z", ".w" }[sValue->bufferIndex % 4]);
    }
    shaderHeader += "\n";
    // Read the shader source from file and prepend the common header
    std::string shaderBody((std::istreambuf_iterator<char>(shaderFile)), std::istreambuf_iterator<char>());
    std::string shaderSource = shaderHeader;
    shaderSource += shaderBody;
    shaderFile.close();
    std::string targetProfile = "ps_5_0"; // Default to pixel shader model 5.0
    if (def->type == ShaderType::Vertex) {
        targetProfile = "vs_5_0";
    } else if (def->type == ShaderType::Pixel) {
        targetProfile = "ps_5_0";
    } else {
        REX::WARN("CompileShader_Internal: Invalid shader type for shader '{}'. Defaulting to pixel shader.", def->id);
    }
    if (!g_rendererData || !g_rendererData->device) {
        REX::WARN("CompileShader_Internal: Renderer device not available. Cannot compile shader '{}'", def->id);
        return false;
    }
    REX::W32::ID3D11Device* device = g_rendererData->device;

    // Try the on-disk compiled-shader cache first. The key is computed from
    // the fully assembled source plus profile/entry/flags plus include-dir
    // contents — any input change naturally produces a miss.
    constexpr uint32_t kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    constexpr std::string_view kEntry = "main";
    const std::string cacheKey = ShaderCache::ComputeKey({
        .assembledSource = shaderSource,
        .profile         = targetProfile,
        .entry           = kEntry,
        .flags           = kCompileFlags,
    });
    ID3DBlob* cachedBlob = nullptr;
    if (ShaderCache::TryLoad(cacheKey, &cachedBlob)) {
        def->compiledShader = cachedBlob;
        REX::INFO("CompileShader_Internal: cache HIT for '{}' ({} bytes)", def->id, def->compiledShader->GetBufferSize());
    } else {
        ID3DBlob* errorBlob = nullptr;
        auto* includeHandler = new ShaderIncludeHandler(); // Custom include handler to resolve #include directives relative to the plugin directory
        HRESULT hr = D3DCompile(
            shaderSource.c_str(),
            shaderSource.size(),
            def->id.c_str(),
            nullptr,
            includeHandler,
            kEntry.data(),
            targetProfile.c_str(),
            kCompileFlags,
            0,
            &def->compiledShader,
            &errorBlob
        );
        delete includeHandler;
        if (!REX::W32::SUCCESS(hr)) {
            if (errorBlob) {
                REX::WARN("CompileShader_Internal: Shader compilation failed: {}", static_cast<const char*>(errorBlob->GetBufferPointer()));
                errorBlob->Release();
            }
            return false;
        }
        if (errorBlob) errorBlob->Release();
        // Only cache on success — failed compiles must not poison future runs.
        ShaderCache::Store(cacheKey, def->compiledShader);
        REX::INFO("CompileShader_Internal: {} compiled successfully! Bytecode size: {} bytes",
                  def->shaderFile.string(), def->compiledShader->GetBufferSize());
    }
    UpdateReplacementResourceUsageFromReflection(def);
    HRESULT hr = S_OK;
    // Set flag to prevent hook from analyzing the shader
    g_isCreatingReplacementShader = true;
    // Create the actual shader object from the compiled bytecode
    if (def->type == ShaderType::Vertex) {
        hr = device->CreateVertexShader(
            def->compiledShader->GetBufferPointer(),
            def->compiledShader->GetBufferSize(),
            nullptr,
            &def->loadedVertexShader
        );
    } else {
        hr = device->CreatePixelShader(
            def->compiledShader->GetBufferPointer(),
            def->compiledShader->GetBufferSize(),
            nullptr,
            &def->loadedPixelShader
        );
    }
    // Reset flag after creation
    g_isCreatingReplacementShader = false;
    if (!REX::W32::SUCCESS(hr)) {
        if (def->type == ShaderType::Vertex) {
            REX::WARN("CompileShader_Internal: Failed to create vertex shader for '{}'", def->id);
        } else {
            REX::WARN("CompileShader_Internal: Failed to create pixel shader for '{}'", def->id);
        }
        return false;
    }
    g_ShaderDB.PublishReplacementForDefinition(def);
    return true;
}

// All provided requirements must match for the function to return true.
bool DoesEntryMatchDefinition_Internal(ShaderDBEntry const& entry, ShaderDefinition* def) {
    // Basic checks
    if (!entry.valid) return false;
    if (!def) return false;
    if (!def->active) return false;
    // Check shader type
    if (def->type == ShaderType::Pixel && entry.type != ShaderType::Pixel) return false;
    if (def->type == ShaderType::Vertex && entry.type != ShaderType::Vertex) return false;
    // Check ShaderUID[s] if specified
    if (!def->shaderUID.empty()) {
        bool uidMatch = false;
        for (const auto& uid : def->shaderUID) {
            if (ToLower(entry.shaderUID) == ToLower(uid)) {
                uidMatch = true;
                break;
            }
        }
        if (!uidMatch) {
            return false;
        }
    }
    // Check hash[es] if specified
    if (def->hash.size() != 0) {
        bool hashMatch = false;
        for (const auto& hash : def->hash) {
            if (entry.hash == hash) {
                hashMatch = true;
                break;
            }
        }
        if (!hashMatch) {
            return false;
        }
    }
    // Check ASM hash if specified
    if (def->asmHash.size() != 0) {
        bool asmHashMatch = false;
        for (const auto& asmHash : def->asmHash) {
            if (entry.asmHash == asmHash) {
                asmHashMatch = true;
                break;
            }
        }
        if (!asmHashMatch) {
            return false;
        }
    }
    // Check size requirement if specified
    if (!def->sizeRequirements.empty()) {
        for (const auto& req : def->sizeRequirements) {
            if (req.op == SizeOp::Equal && entry.size != req.value) return false;
            if (req.op == SizeOp::Greater && entry.size <= req.value) return false;
            if (req.op == SizeOp::Less && entry.size >= req.value) return false;
        }
    }
    // Check constant buffer sizes
    for (const auto& [size, slot] : def->bufferSizes) {
        const auto expectedSize = static_cast<std::uint32_t>(size);
        // Handle size@ without slot (any slot)
        if (slot < 0) {
            bool anySlotMatches = false;
            for (int i = 0; i < 14; i++) {
                if (entry.expectedCBSizes[i] == expectedSize) {
                    anySlotMatches = true;
                    break;
                }
            }
            if (!anySlotMatches) return false;
        // Handle size@slot (specific slot)
        } else if (slot >= 0 && slot < 14) {
        if (entry.expectedCBSizes[slot] != expectedSize)
            return false;
        }
    }
    // Check texture slots
    if (def->textureSlotMask != 0 && ((entry.textureSlotMask & def->textureSlotMask) != def->textureSlotMask))
        return false;
    // Check texture dimensions
    if (def->textureDimensionMask != 0 && ((entry.textureDimensionMask & def->textureDimensionMask) != def->textureDimensionMask))
        return false;
    // Check input texture count if specified
    if (!def->inputTextureCountRequirements.empty()) {
        for (const auto& req : def->inputTextureCountRequirements) {
            if (req.op == SizeOp::Equal && entry.inputTextureCount != req.value) return false;
            if (req.op == SizeOp::Greater && entry.inputTextureCount <= req.value) return false;
            if (req.op == SizeOp::Less && entry.inputTextureCount >= req.value) return false;
        }
    }
    // Check input count if specified
    if (!def->inputCountRequirements.empty()) {
        for (const auto& req : def->inputCountRequirements) {
            if (req.op == SizeOp::Equal && entry.inputCount != req.value) return false;
            if (req.op == SizeOp::Greater && entry.inputCount <= req.value) return false;
            if (req.op == SizeOp::Less && entry.inputCount >= req.value) return false;
        }
    }
    // Check input mask if specified
    if (def->inputMask != 0 && (entry.inputMask & def->inputMask) != def->inputMask)
        return false;
    // Check output count if specified
    if (!def->outputCountRequirements.empty()) {
        for (const auto& req : def->outputCountRequirements) {
            if (req.op == SizeOp::Equal && entry.outputCount != req.value) return false;
            if (req.op == SizeOp::Greater && entry.outputCount <= req.value) return false;
            if (req.op == SizeOp::Less && entry.outputCount >= req.value) return false;
        }
    }
    // Check output mask if specified
    if (def->outputMask != 0 && (entry.outputMask & def->outputMask) != def->outputMask)
        return false;
    // It matches all provided requirements
    return true;
}

// Dump the original shader bytecode to a file for analysis
void DumpOriginalShader_Internal(ShaderDBEntry const& entry, ShaderDefinition* def) {
    if (!def->dump || !entry.IsValid() || entry.IsDumped()) return;
    // Schedule the dump on the game's task queue
    if (g_taskInterface) {
        // Capture entry and def by value to ensure they remain valid in the task
        g_taskInterface->AddTask([type=entry.type,
                                  shaderUID=entry.shaderUID,
                                  hash=entry.hash,
                                  asmHash=entry.asmHash,
                                  size=entry.size,
                                  bytecode=entry.bytecode,
                                  expectedCBSizes=[&entry]() { 
                                        std::array<std::uint32_t, 14> arr; 
                                        std::memcpy(arr.data(), entry.expectedCBSizes, sizeof(entry.expectedCBSizes)); 
                                        return arr; 
                                    }(),
                                  textureSlots=entry.textureSlots,
                                  textureDimensions=entry.textureDimensions,
                                  textureSlotMask=entry.textureSlotMask,
                                  textureDimensionMask=entry.textureDimensionMask,
                                  inputTextureCount=entry.inputTextureCount,
                                  inputCount=entry.inputCount,
                                  inputMask=entry.inputMask,
                                  outputCount=entry.outputCount,
                                  outputMask=entry.outputMask,
                                  def](){
            std::filesystem::path dumpPath = g_pluginPath / "ShaderEngineDumps" / def->id;
            std::filesystem::create_directories(dumpPath);
            if (def->dump && !bytecode.empty()) {
                std::string binFilename = std::format("{}.bin", shaderUID);
                std::string asmFilename = std::format("{}.asm", shaderUID);
                std::string logFilename = std::format("{}.txt", shaderUID);
                std::filesystem::path binPath = dumpPath / binFilename;
                std::filesystem::path asmPath = dumpPath / asmFilename;
                std::filesystem::path logPath = dumpPath / logFilename;
                // Check if files already exist
                if (DEBUGGING && std::filesystem::exists(binPath)) {
                    REX::WARN("DumpOriginalShader_Internal: Binary file already exists, skipping: {}", binPath.string());
                    return;
                }
                // Dump bytecode to binary file
                std::ofstream binFile(binPath, std::ios::binary);
                binFile.write(reinterpret_cast<const char*>(bytecode.data()), bytecode.size());
                binFile.close();
                // Also disassemble to text
                ID3DBlob* disassembly = nullptr;
                HRESULT hr = D3DDisassemble(bytecode.data(), bytecode.size(), 0, nullptr, &disassembly);
                if (REX::W32::SUCCESS(hr) && disassembly) {
                    std::ofstream asmFile(asmPath);
                    asmFile.write(static_cast<const char*>(disassembly->GetBufferPointer()), disassembly->GetBufferSize());
                    asmFile.close();
                    disassembly->Release();
                }
                // Write a log file in the format of the Shader.ini
                std::ofstream logFile(logPath);
                // Write INI section header
                logFile << "[" << def->id << "]" << std::endl;
                logFile << "active=true" << std::endl;
                logFile << "priority=0" << std::endl;
                logFile << "type=" << (type == ShaderType::Pixel ? "ps" : "vs") << std::endl;
                logFile << "shaderUID=" << shaderUID << std::endl;
                logFile << "hash=0x" << std::hex << std::uppercase << hash << std::dec << std::endl;
                logFile << "asmHash=0x" << std::hex << std::uppercase << asmHash << std::dec << std::endl;
                // Size as exact match in parentheses
                logFile << "size=(" << size << ")" << std::endl;
                // Buffer sizes in format: size@slot,size@slot
                logFile << "buffersize=";
                bool firstBuffer = true;
                for (int i = 0; i < 14; ++i) {
                    if (expectedCBSizes[i] > 0) {
                        if (!firstBuffer) logFile << ",";
                        logFile << expectedCBSizes[i] << "@" << i;
                        firstBuffer = false;
                    }
                }
                logFile << std::endl;
                // Textures in format: 0,1,2,...
                logFile << "textures=";
                if (!textureSlots.empty()) {
                    bool firstSlot = true;
                    for (const auto& slot : textureSlots) {
                        if (!firstSlot) logFile << ",";
                        logFile << slot;
                        firstSlot = false;
                    }
                }
                logFile << std::endl;
                // Texture dimensions in format: dimension@slot
                logFile << "textureDimensions=";
                if (!textureDimensions.empty()) {
                    bool firstDim = true;
                    for (const auto& [dimension, slot] : textureDimensions) {
                        if (!firstDim) logFile << ",";
                        logFile << dimension << "@" << slot;
                        firstDim = false;
                    }
                }
                logFile << std::endl;
                // Bitmasks
                logFile << "textureSlotMask=0x" << std::hex << std::uppercase << textureSlotMask << std::dec << std::endl;
                logFile << "textureDimensionMask=0x" << std::hex << std::uppercase << textureDimensionMask << std::dec << std::endl;
                // Counts in parentheses
                logFile << "inputTextureCount=(" << inputTextureCount << ")" << std::endl;
                logFile << "inputcount=(" << inputCount << ")" << std::endl;
                logFile << "inputMask=0x" << std::hex << std::uppercase << inputMask << std::dec << std::endl;
                logFile << "outputcount=(" << outputCount << ")" << std::endl;
                logFile << "outputMask=0x" << std::hex << std::uppercase << outputMask << std::dec << std::endl;
                // Shader file (empty, user needs to add)
                logFile << "shader=;" << shaderUID << "_replacement.hlsl" << std::endl;
                logFile << "log=true" << std::endl;
                logFile << "dump=true" << std::endl;
                // Close section
                logFile << "[/" << def->id << "]" << std::endl;
                logFile.close();
                if (DEBUGGING)
                    REX::INFO("DumpOriginalShader_Internal: Dumped original shader for ID {} to disk for analysis. Binary: {}, Disassembly: {}, Log: {}", def->id, binFilename, asmFilename, logFilename);
            } else {
                if (DEBUGGING)
                    REX::WARN("DumpOriginalShader_Internal: Failed to dump shader for ID {} - either dumping is disabled or bytecode is not available.", def->id);
            }
        });
    } else {
        REX::WARN("DumpOriginalShader_Internal: Failed to dump shader for ID {} - task interface not available.", def->id);
    }
}


// Disassemble the shader bytecode and parse it to find out details about the shader
// Normal reflection API does not provide all the info we need and it unreliable
bool ReflectShader_Internal(ShaderDBEntry& entry) {
    if (entry.bytecode.empty()) return false;
    // We disassemble the shader and parse it manually to fill in our entry data.
    ID3DBlob* disassembly = nullptr;
    HRESULT hr = D3DDisassemble(entry.bytecode.data(), entry.bytecode.size(), 0, nullptr, &disassembly);
    if (!REX::W32::SUCCESS(hr) || !disassembly) {
        REX::WARN("ReflectShader_Internal: Failed to disassemble shader bytecode for reflection.");
        return false;
    }
    std::string disasmStr(static_cast<const char*>(disassembly->GetBufferPointer()), disassembly->GetBufferSize());
    // Define regexes as static once
    static const auto regexFlags = std::regex_constants::optimize | std::regex_constants::icase;
        // Catch instructions - only real opcodes, anchored at line start
    static std::regex instrRegex(
        R"(^\s*(add|sub|mul|div|mad|max|min|dp2|dp3|dp4|rsq|sqrt|"
        "and|or|xor|not|lt|gt|le|ge|eq|ne|"
        "mov(?:c|_sat)?|sample(?:_indexable)?|"
        "loop|endloop|if|else|endif|break(?:c)?|ret)\b)",
        regexFlags);
    // Catch t# registers
    static std::regex texRegex(R"(dcl_resource_(\w+)\s*(?:\([^)]*\))?\s*(?:\([^)]+\))?\s+t(\d+))", regexFlags);
    // Catch v# registers (broad match for any dcl_input flavor)
    static std::regex inputRegex(R"(dcl_input[^\s]*\s+v(\d+))", regexFlags);
    // Catch cb# registers
    static std::regex cbRegex(R"(dcl_constantbuffer\s+cb(\d+)\[(\d+)\])", regexFlags);
    // Catch o# registers (broad match for any dcl_output flavor)
    static std::regex outputRegex(R"(dcl_output[^\s]*\s+o(\d+))", regexFlags);
    // Parse the disassembly text
    std::istringstream iss(disasmStr);
    std::string line;
    // Clear the buffers before filling them in case this is called multiple times for the same entry (e.g., if we analyze both pixel and vertex shader for the same entry)
    entry.shaderUID = "";
    entry.asmHash = 0;
    entry.textureSlots.clear();
    entry.textureDimensions.clear();
    entry.textureSlotMask = 0;
    entry.textureDimensionMask = 0;
    entry.inputTextureCount = 0;
    entry.inputCount = 0;
    entry.inputMask = 0;
    // Manual loop for safety to avoid sizeof pointer issues
    for (int i = 0; i < 14; ++i) entry.expectedCBSizes[i] = 0;
    entry.outputCount = 0;
    entry.outputMask = 0;
    std::string asmConcat;
    int inputTextureCount = 0;
    int inputCount = 0;
    int outputCount = 0;
    while (std::getline(iss, line)) {
        // Look for instructions to get a rough idea of shader complexity
        std::smatch match;
        if (std::regex_search(line, match, instrRegex)) {
            asmConcat += match[1].str() + ";"; // Add the opcode to a concatenated string for hashing
        }
        // Look for texture declarations to detect texture slots and dimensions (mainly pixel shaders)
        // Example: "dcl_resource_texture2d (float,float,float,float) t0"
        // Dimensions from d3dcommon.h
        // D3D11_SRV_DIMENSION_UNKNOWN = 0
        // D3D11_SRV_DIMENSION_BUFFER = 1
        // D3D11_SRV_DIMENSION_TEXTURE1D = 3
        // D3D11_SRV_DIMENSION_TEXTURE2D = 4
        // D3D11_SRV_DIMENSION_TEXTURE2DMS = 6
        // D3D11_SRV_DIMENSION_TEXTURE3D = 7
        // D3D11_SRV_DIMENSION_TEXTURECUBE = 8
        // D3D11_SRV_DIMENSION_TEXTURE1DARRAY = 4
        // D3D11_SRV_DIMENSION_TEXTURE2DARRAY = 5
        // D3D11_SRV_DIMENSION_TEXTURECUBEARRAY = 11
        // Texture / Resource declaration parsing (mainly pixel shaders)
        if (std::regex_search(line, match, texRegex)) {
            std::string texType = match[1];      // "texture1d"
            int slot = std::stoi(match[2]);      // 4
            int dimension = 0;
            if (texType == "texture2d") dimension = 4;
            else if (texType == "texture2dms") dimension = 6;
            else if (texType == "texture2darray") dimension = 5;
            else if (texType == "texturecube") dimension = 8;
            else if (texType == "texturecubearray") dimension = 11;
            else if (texType == "texture3d") dimension = 7;
            else if (texType == "texture1d") dimension = 3;
            else if (texType == "buffer") dimension = 1;
            else if (texType == "raw" || texType == "structured") dimension = 0; // For Vertex shaders
            else dimension = 0; // unknown or unsupported type
            entry.textureSlots.push_back(slot); // slot
            entry.textureDimensions.push_back({dimension, slot});
            entry.textureSlotMask |= (1u << slot);
            if (dimension < 32) {
                entry.textureDimensionMask |= (1u << dimension);
            }
            inputTextureCount++;
            continue;
        }
        // Input declaration parsing (mainly vertex shaders)
        // Look for input like POSITION or TEXCOORD to detect input count (mainly vertex shaders)
        if (std::regex_search(line, match, inputRegex)) {
            int regIndex = std::stoi(match[1].str());
            entry.inputMask |= (1u << regIndex);
            inputCount++;
            continue;
        }
        // Constant Buffer declaration parsing to detect expected CB sizes for matching (pixel and vertex shaders)
        // Example: "dcl_constantbuffer CB0[4], immediateIndexed"
        if (std::regex_search(line, match, cbRegex)) {
            int slot = std::stoi(match[1].str());
            int sizeInDwords = std::stoi(match[2].str());
            if (slot >= 0 && slot < 14) {
                entry.expectedCBSizes[slot] = sizeInDwords * 16;
            }
            continue;
        }
        // Look for output declarations to detect output count (pixel and vertex shaders)
        // Example: "dcl_output o0.xyzw"
        if (std::regex_search(line, match, outputRegex)) {
            int outputIndex = std::stoi(match[1].str());
            entry.outputMask |= (1u << outputIndex);
            outputCount++;
        }
    }
    // Clean up
    disassembly->Release();
    entry.asmHash = static_cast<std::uint32_t>(std::hash<std::string_view>{}(asmConcat));
    entry.inputTextureCount = inputTextureCount;
    entry.inputCount = inputCount;
    entry.outputCount = outputCount;
    entry.shaderUID = std::format("{}{:08X}I{}O{}",
        entry.type == ShaderType::Pixel ? "PS" : "VS",
        entry.asmHash,
        entry.inputCount,
        entry.outputCount);
    return true;
}

// Render-thread half of the HLSL hot-reload pipeline. The watcher thread
// just flips a flag (HlslFileWatcher::Check); we do the actual D3D11
// Release / replacement-cache eviction here, on the same thread that issues
// draws, so we never race the immediate context. Called from MyPSSetShader
// and MyVSSetShader before they reach CompileShader_Internal.
void MaybeApplyHlslHotReload_Internal(ShaderDefinition* def) {
    if (!def || !def->hlslFileWatcher) return;
    if (!def->hlslFileWatcher->ConsumeReloadRequest()) return;
    // Take the per-def compile mutex so an in-flight precompile (or a
    // concurrent on-demand render-thread compile) cannot publish bytecode
    // that we're about to drop, leaving us with a stale but newly-cached
    // replacement.
    if (!def->compileMutex) def->compileMutex = std::make_unique<std::mutex>();
    std::lock_guard compileLock(*def->compileMutex);
    if (def->loadedPixelShader)  { def->loadedPixelShader->Release();  def->loadedPixelShader  = nullptr; }
    if (def->loadedVertexShader) { def->loadedVertexShader->Release(); def->loadedVertexShader = nullptr; }
    if (def->compiledShader)     { def->compiledShader->Release();     def->compiledShader     = nullptr; }
    def->usesGFXInjected = false;
    def->usesGFXDrawTag = false;
    def->usesGFXModularFloats = false;
    def->usesGFXModularInts = false;
    def->usesGFXModularBools = false;
    def->buggy = false;
    g_ShaderDB.ClearReplacementsForDefinition(def);
    REX::INFO("MaybeApplyHlslHotReload_Internal: dropped compiled state for definition '{}'", def->id);
}

// Orchestrator for hot INI reload - rematches all ShaderDB entries against current definitions
void RematchAllShaders_Internal() {
    std::unique_lock lockDB(g_ShaderDB.mutex);  // Write lock on ShaderDB
    if (DEBUGGING)
        REX::INFO("RematchAllShaders_Internal: Rematching {} pixel shaders and vertex shaders...", g_ShaderDB.entries.size());
    int matchedPS = 0;
    int matchedVS = 0;
    // Iterate definitions in priority order (already sorted)
    std::shared_lock lock(g_shaderDefinitions.mutex);
    for (ShaderDefinition* def : g_shaderDefinitions.definitions) {
        if (!def->active) continue;
        // Check all pixel shaders for this definition
        for (auto& [shader, entry] : g_ShaderDB.entries) {
            if (entry.IsMatched()) continue;
            if (DoesEntryMatchDefinition_Internal(entry, def)) {
                if (DEBUGGING)
                    REX::INFO("RematchAllShaders_Internal: Matched {} shader with hash {:08X} and size {} bytes to definition '{}'", (def->type == ShaderType::Pixel ? "pixel" : "vertex"), entry.hash, entry.size, def->id);
                entry.matchedDefinition = def;
                entry.SetMatched(true);
                if (def->type == ShaderType::Pixel) {
                    matchedPS++;
                } else if (def->type == ShaderType::Vertex) {
                    matchedVS++;
                }
                if (DEVELOPMENT && def->log) {
                    REX::INFO("RematchAllShaders_Internal: ------------------------------------------------");
                    REX::INFO("RematchAllShaders_Internal: Found matching shader definition '{}' for {} shader with ShaderUID '{}'.", def->id, entry.type == ShaderType::Vertex ? "Vertex" : "Pixel", entry.shaderUID);
                    REX::INFO("RematchAllShaders_Internal: Shader Hash: {:08X}", entry.hash);
                    REX::INFO("RematchAllShaders_Internal: Shader Size: {} bytes", entry.size);
                    REX::INFO("RematchAllShaders_Internal: Shader ASM Hash: {:08X}", entry.asmHash);
                    REX::INFO(" - Shader CB Sizes: {},{},{},{},{},{},{},{},{},{},{},{},{},{}", 
                        entry.expectedCBSizes[0], entry.expectedCBSizes[1], entry.expectedCBSizes[2], entry.expectedCBSizes[3],
                        entry.expectedCBSizes[4], entry.expectedCBSizes[5], entry.expectedCBSizes[6], entry.expectedCBSizes[7],
                        entry.expectedCBSizes[8], entry.expectedCBSizes[9], entry.expectedCBSizes[10], entry.expectedCBSizes[11],
                        entry.expectedCBSizes[12], entry.expectedCBSizes[13]);
                    REX::INFO(" - Shader Texture Register Slots: {}", entry.textureSlots.empty() ? "None" : "");
                    for (const auto& slot : entry.textureSlots) {
                        REX::INFO("   - Slot: t{}", slot);
                    }
                    REX::INFO(" - Shader Texture Dimensions: {}", entry.textureDimensions.empty() ? "None" : "");
                    for (const auto& [dimension, slot] : entry.textureDimensions) {
                        REX::INFO("   - Dimension: {}, Slot: t{}", dimension, slot);
                    }
                    REX::INFO(" - Shader Texture Usage Bitmask: 0x{:08X}", entry.textureSlotMask);
                    REX::INFO(" - Shader Texture Dimension Bitmask: 0x{:08X}", entry.textureDimensionMask);
                    REX::INFO(" - Shader Input Texture Count: {}", entry.inputTextureCount != -1 ? std::to_string(entry.inputTextureCount) : "X");
                    REX::INFO(" - Shader Input Count: {}", entry.inputCount != -1 ? std::to_string(entry.inputCount) : "X");
                    REX::INFO(" - Shader Input Mask: 0x{:08X}", entry.inputMask);
                    REX::INFO(" - Shader Output Count: {}", entry.outputCount != -1 ? std::to_string(entry.outputCount) : "X");
                    REX::INFO(" - Shader Output Mask: 0x{:08X}", entry.outputMask);
                    REX::INFO("RematchAllShaders_Internal: ------------------------------------------------");
                }
                if (DEVELOPMENT && def->dump && !entry.IsDumped()) {
                    DumpOriginalShader_Internal(entry, def);
                    entry.SetDumped(true);
                }
            }
        }
    }
    if (DEBUGGING)
        REX::INFO("RematchAllShaders_Internal: Matched {} pixel shaders and {} vertex shaders", matchedPS, matchedVS);
    CustomPass::g_registry.InvalidateDrawPassCache();
}
