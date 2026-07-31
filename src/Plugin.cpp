#include <Global.h>
#include <PCH.h>
#include <CustomPass.h>
#include <InteriorDeferredLighting.h>
#include "d3dhooks.h"
#include <hooks.h>
#include <PhaseTelemetry.h>
#include <ShadowTelemetry.h>
#include <LightCullPolicy.h>
#include <ShaderPipeline.h>
#include <ShaderResources.h>
#include <ShadowUpgrade.h>

#include <optional>

namespace RE
{
    class BSLODTriShape :
        public BSTriShape
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSLODTriShape };
    };

    class BSLODMultiIndexTriShape :
        public BSTriShape
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSLODMultiIndexTriShape };
    };

    class BSMeshLODTriShape :
        public BSTriShape
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSMeshLODTriShape };
    };

    class BSPackedCombinedGeomDataExtra :
        public NiExtraData
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSPackedCombinedGeomDataExtra };
    };

    class BSPackedCombinedSharedGeomDataExtra :
        public NiExtraData
    {
    public:
        static constexpr auto Ni_RTTI{ Ni_RTTI::BSPackedCombinedSharedGeomDataExtra };
    };
}

// --- Variables ---

// Global Singletons
RE::BSGraphics::RendererData* g_rendererData = nullptr;
HWND g_outputWindow = nullptr;
// Global Shader DB
ShaderDB g_ShaderDB = {};
// Global trackers of current shader
int g_currentTextureDSIndices[128] = { -1 }; 
// Tell MyCreatePixelShader to skip analysing the shader when creating replacement shaders to avoid infinite recursion
thread_local bool g_isCreatingReplacementShader = false;
// Last original (pre-replacement) pixel shader observed in MyPSSetShader.
// Used by the actor-tag debug logger to attribute draws to a shader UID.
// Externally visible for CustomPass.cpp's BeforeDrawForMatchedDef trigger,
// which fires from MyDraw* hooks and needs to identify the currently bound
// original (pre-replacement) PS to look up its matched ShaderDefinition.
std::atomic<REX::W32::ID3D11PixelShader*> g_currentOriginalPixelShader{ nullptr };
static thread_local const CustomPass::DrawPassBatch* g_armedCustomPassDrawBatch = nullptr;
static thread_local std::uint64_t g_armedCustomPassDrawBatchGeneration = 0;
// Global custom buffer data structure instance for updating CB13
GFXBoosterAccessData g_customBufferData = {};
DrawTagData g_drawTagData = {};
REX::W32::ID3D11Buffer* g_drawTagSRVBuffer = nullptr;
REX::W32::ID3D11ShaderResourceView* g_drawTagSRV = nullptr;

void ArmCustomPassDrawBatch(REX::W32::ID3D11PixelShader* originalPS)
{
    if (!SHADERENGINE_EFFECTS_ON) {
        g_armedCustomPassDrawBatch = nullptr;
        g_armedCustomPassDrawBatchGeneration = 0;
        return;
    }
    g_armedCustomPassDrawBatch =
        CustomPass::g_registry.ResolveDrawPassBatchForShader(
            originalPS,
            &g_armedCustomPassDrawBatchGeneration);
}

bool FireArmedCustomPassDrawBatch(REX::W32::ID3D11DeviceContext* context, const char* source)
{
    if (!SHADERENGINE_EFFECTS_ON || !g_armedCustomPassDrawBatch) {
        return false;
    }
    RefreshCustomBufferForCustomPass();
    return CustomPass::g_registry.FireResolvedDrawBatch(
        context,
        g_armedCustomPassDrawBatch,
        g_armedCustomPassDrawBatchGeneration,
        source);
}



// Ring of distinct buffer+SRV pairs so each draw gets its own backing.
// F4 batches draws into BSGraphics command queues; with a single shared
// buffer, all queued draws would resolve to whatever value was Mapped last
// before flush ? appearing as stale tags.
static constexpr UINT DRAWTAG_RING_SIZE = 1024;
static std::array<REX::W32::ID3D11Buffer*, DRAWTAG_RING_SIZE> g_drawTagRingBuffers{};
static std::array<REX::W32::ID3D11ShaderResourceView*, DRAWTAG_RING_SIZE> g_drawTagRingSRVs{};
static std::atomic<UINT> g_drawTagRingCursor{ 0 };

// --- Static per-tag resources for the command-buffer-recording path ---
//
// BSShader::BuildCommandBuffer hook records one of these wrappers per pass.
// The recorded wrapper pointer is captured into the command buffer and
// dereferenced at replay time (potentially many frames later, on whatever
// thread the engine flushes), so the SRV must be a long-lived immutable
// resource ? ring slots would race because they get reused per-draw.
//
// Layout matches what ProcessCommandBuffer reads for kind=0:
//   +0x00  unused
//   +0x08  ID3D11ShaderResourceView*  (passed as ppSRVs to PSSetShaderResources)
//   +0x30  fence count, must be 0 to skip priority/wait branches
struct alignas(16) FakeStructuredResource
{
    void*                                    pad00 = nullptr;
    std::atomic<REX::W32::ID3D11ShaderResourceView*> srv = nullptr;
    char                                     pad10[0x20] = {};
    std::uint32_t                            fenceCount = 0;
    char                                     pad34[12] = {};
};
static_assert(sizeof(FakeStructuredResource) == 64);
static_assert(sizeof(std::atomic<REX::W32::ID3D11ShaderResourceView*>) == sizeof(REX::W32::ID3D11ShaderResourceView*));
static_assert(offsetof(FakeStructuredResource, srv) == 0x08);
static_assert(offsetof(FakeStructuredResource, fenceCount) == 0x30);

// Recorded entry layout that ProcessCommandBuffer iterates as a 16-byte
// table after the textures section in each command buffer.
struct CommandBufferShaderResource
{
    void*           resourceWrapper;  // +0x00
    std::uint8_t    slot;             // +0x08
    std::uint8_t    stage;            // +0x09 (0=VS, 1=PS)
    std::uint8_t    kind;             // +0x0A (0 = wrapper+0x08 path)
    std::uint8_t    pad0B = 0;
    std::uint32_t   pad0C = 0;
};
static_assert(sizeof(CommandBufferShaderResource) == 16);

struct StaticDrawTagKey
{
    std::uint32_t materialTag = 0;
    std::uint32_t isHead = 0;
    std::uint32_t raceGroupMask = 0;
    std::uint32_t raceFlags = 0;

    bool operator==(const StaticDrawTagKey& rhs) const noexcept
    {
        return materialTag == rhs.materialTag &&
               isHead == rhs.isHead &&
               raceGroupMask == rhs.raceGroupMask &&
               raceFlags == rhs.raceFlags;
    }
};

struct StaticDrawTagKeyHash
{
    std::size_t operator()(const StaticDrawTagKey& key) const noexcept
    {
        std::size_t h = std::hash<std::uint32_t>{}(key.materialTag);
        auto mix = [&h](std::uint32_t v) {
            h ^= std::hash<std::uint32_t>{}(v) + 0x9e3779b9u + (h << 6) + (h >> 2);
        };
        mix(key.isHead);
        mix(key.raceGroupMask);
        mix(key.raceFlags);
        return h;
    }
};

struct StaticDrawTagResource
{
    REX::W32::ID3D11Buffer* buffer = nullptr;
    REX::W32::ID3D11ShaderResourceView* srv = nullptr;
};

static std::unordered_map<StaticDrawTagKey, StaticDrawTagResource, StaticDrawTagKeyHash> g_staticDrawTagResources;
static std::mutex g_staticDrawTagResourcesMutex;

static REX::W32::ID3D11Buffer*              g_actorTagBuffer = nullptr;
static REX::W32::ID3D11ShaderResourceView*  g_actorTagSRV = nullptr;
static REX::W32::ID3D11Buffer*              g_unknownTagBuffer = nullptr;
static REX::W32::ID3D11ShaderResourceView*  g_unknownTagSRV = nullptr;
// Head/facegen variant: same materialTag as kActor (1) but with isHead = 1
// in the structured buffer so PS code can branch on facegen vs. equipment.
static REX::W32::ID3D11Buffer*              g_actorHeadTagBuffer = nullptr;
static REX::W32::ID3D11ShaderResourceView*  g_actorHeadTagSRV = nullptr;
static FakeStructuredResource               g_actorTagWrapper{};
static FakeStructuredResource               g_unknownTagWrapper{};
static FakeStructuredResource               g_actorHeadTagWrapper{};
static std::unordered_map<RE::BSGeometry*, std::unique_ptr<FakeStructuredResource>> g_commandBufferDrawTagWrappers;
static std::shared_mutex                    g_commandBufferDrawTagWrappersLock;
static std::mutex                           g_tagWrapperInitMutex;
static bool                                 g_tagWrapperResourcesReady = false;


static REX::W32::ID3D11ShaderResourceView* UpdateDrawTagBuffer(
    REX::W32::ID3D11DeviceContext* context,
    float materialTag,
    float isHead,
    std::uint32_t raceGroupMask,
    std::uint32_t raceFlags);
static void BindDrawTagForCurrentDraw(REX::W32::ID3D11DeviceContext* context, bool force = false);

namespace
{
    enum class DrawMaterialTag : std::uint32_t
    {
        kUnknown = 0,
        kActor = 1
    };

    constexpr std::uint32_t kDrawTagRaceResolved = 1u << 0;

    struct DrawTagClassification
    {
        float materialTag = static_cast<float>(DrawMaterialTag::kUnknown);
        float isHead = 0.0f;
        std::uint32_t raceGroupMask = 0;
        std::uint32_t raceFlags = 0;
    };

    std::uint32_t CountPassChain(BSRenderPassLayout* head)
    {
        std::uint32_t count = 0;
        for (auto* pass = head; pass && count < 512; pass = pass->passGroupNext) {
            ++count;
        }
        return count;
    }

    bool IsShadowWorkTelemetryActive() noexcept
    {
        return ShadowTelemetry::g_mode.load(std::memory_order_relaxed) == ShadowTelemetry::Mode::On &&
               ShadowTelemetry::IsInShadowMap();
    }

    void DecodeCommandBufferData(void* cbData, ShadowTelemetry::WorkTarget& target)
    {
        if (!cbData) {
            return;
        }

        auto** records = static_cast<void**>(cbData);
        constexpr std::uint32_t kMaxRecords = 4096;
        for (std::uint32_t i = 0; i < kMaxRecords; ++i) {
            void* record = records[i];
            if (!record) {
                break;
            }

            if (!target.commandBuffer) {
                target.commandBuffer = record;
            }

            auto* bytes = static_cast<std::uint8_t*>(record);
            const auto drawCount = static_cast<std::uint32_t>(bytes[0x04]);
            const auto srvRecordCount = static_cast<std::uint32_t>(bytes[0x06]);
            auto* drawEntries = reinterpret_cast<const std::uint32_t*>(bytes + 0x3C);
            ++target.cbRecordCount;
            target.cbDrawCount += drawCount;
            target.cbSrvRecordCount += srvRecordCount;
            for (std::uint32_t j = 0; j < drawCount; ++j) {
                if (drawEntries[j] != 0) {
                    ++target.cbNonZeroDrawCount;
                }
            }
            if (drawCount > target.cbMaxDrawCount) {
                target.cbMaxDrawCount = drawCount;
            }
            if (srvRecordCount > target.cbMaxSrvRecordCount) {
                target.cbMaxSrvRecordCount = srvRecordCount;
            }
        }
    }

    ShadowTelemetry::WorkTarget MakeShadowPassWorkTarget(
        void* owner,
        BSRenderPassLayout* head,
        void* cbData,
        std::int32_t passGroupIdx,
        std::uint32_t subIdx,
        bool allowAlpha)
    {
        ShadowTelemetry::WorkTarget target{};
        target.owner = owner;
        target.head = head;
        target.cbData = cbData;
        target.commandBuffer = nullptr;
        target.geometry = head ? static_cast<void*>(head->geometry) : nullptr;
        target.shader = head ? static_cast<void*>(head->shader) : nullptr;
        target.shaderProperty = head ? static_cast<void*>(head->shaderProperty) : nullptr;
        target.techniqueID = head ? head->techniqueID : 0;
        target.passGroupIdx = passGroupIdx;
        target.subIdx = subIdx;
        target.chainLen = CountPassChain(head);
        target.allowAlpha = allowAlpha;
        target.commandBuffer = head ? head->commandBuffer : nullptr;
        return target;
    }

    class ScopedShadowWork
    {
    public:
        ScopedShadowWork(ShadowTelemetry::WorkKind kind, const ShadowTelemetry::WorkTarget& target) :
            kind_(kind),
            active_(ShadowTelemetry::BeginShadowWork(kind, target))
        {}

        ~ScopedShadowWork()
        {
            if (active_) {
                ShadowTelemetry::EndShadowWork(kind_);
            }
        }

        ScopedShadowWork(const ScopedShadowWork&) = delete;
        ScopedShadowWork& operator=(const ScopedShadowWork&) = delete;

    private:
        ShadowTelemetry::WorkKind kind_;
        bool active_;
    };

    thread_local ShadowTelemetry::WorkTarget tl_processCommandBufferTarget{};
    thread_local bool tl_processCommandBufferTargetActive = false;
    std::shared_mutex g_shadowGeometryClassLock;
    std::unordered_map<RE::BSGeometry*, bool> g_shadowGeometryIsPrecombine;
    std::unordered_map<RE::NiAVObject*, bool> g_shadowObjectHasPackedCombinedExtra;
    std::unordered_map<RE::NiAVObject*, bool> g_shadowObjectHasDynamicBSXFlags;
    struct ShadowPassClassification
    {
        RE::BSGeometry* geometry = nullptr;
        std::uint8_t flags = 0;
    };
    std::shared_mutex g_shadowPassClassLock;
    std::unordered_map<BSRenderPassLayout*, ShadowPassClassification> g_shadowPassClassifications;

    constexpr std::uint8_t kShadowPassClassStatic = 1u << 0;
    constexpr std::uint32_t kBSXAnimated = 0x1;
    constexpr std::uint32_t kBSXHavok = 0x2;
    constexpr std::uint32_t kBSXRagdoll = 0x4;
    constexpr std::uint32_t kBSXDynamic = 0x40;      // 64 decimal in NIF tools
    constexpr std::uint32_t kBSXArticulated = 0x80;
    constexpr std::uint32_t kShadowDynamicBSXMask =
        kBSXAnimated | kBSXHavok | kBSXRagdoll | kBSXDynamic | kBSXArticulated;
    constexpr std::uint32_t kMaxShadowParentScan = 2;
    constexpr std::size_t kMaxShadowGeometryClassCache = 262144;
    constexpr std::size_t kMaxShadowPassClassCache = 262144;
    constexpr bool kLegacyFinishLevelShadowSplitEnabled = false;

    std::uint32_t GetBSXFlags(RE::NiObjectNET* object, std::uint32_t mask)
    {
        if (!object) {
            return 0;
        }

        using GetFlags_t = std::uint32_t (*)(RE::NiObjectNET*, std::uint32_t);
        static REL::Relocation<GetFlags_t> getFlags{ REL::ID{ 1036263, 0 } };  // BSXFlags::GetFlags, FO4 1.10.163
        return getFlags(object, mask);
    }

    bool IsPackedCombinedExtraData(RE::NiExtraData* extra)
    {
        if (!extra) {
            return false;
        }

        if (netimmerse_cast<RE::BSPackedCombinedGeomDataExtra*>(extra) ||
            netimmerse_cast<RE::BSPackedCombinedSharedGeomDataExtra*>(extra)) {
            return true;
        }

        static const RE::BSFixedString packedGeomName("BSPackedCombinedGeomDataExtra");
        static const RE::BSFixedString packedSharedName("BSPackedCombinedSharedGeomDataExtra");
        return extra->name == packedGeomName || extra->name == packedSharedName;
    }

    bool ComputePackedCombinedExtra(RE::NiAVObject* object)
    {
        if (!object) {
            return false;
        }

        auto* extras = object->extra;
        if (!extras) {
            return false;
        }

        for (auto it = extras->begin(); it != extras->end(); ++it) {
            if (IsPackedCombinedExtraData(*it)) {
                return true;
            }
        }
        return false;
    }

    bool HasPackedCombinedExtra(RE::NiAVObject* object)
    {
        if (!object) {
            return false;
        }

        {
            std::shared_lock lock(g_shadowGeometryClassLock);
            auto it = g_shadowObjectHasPackedCombinedExtra.find(object);
            if (it != g_shadowObjectHasPackedCombinedExtra.end()) {
                return it->second;
            }
        }

        const bool hasExtra = ComputePackedCombinedExtra(object);
        std::unique_lock lock(g_shadowGeometryClassLock);
        g_shadowObjectHasPackedCombinedExtra[object] = hasExtra;
        return hasExtra;
    }

    bool ComputeDynamicBSXFlags(RE::NiAVObject* object)
    {
        if (object && object->controllers) {
            return true;
        }
        return GetBSXFlags(object, kShadowDynamicBSXMask) != 0;
    }

    bool HasDynamicBSXFlags(RE::NiAVObject* object)
    {
        if (!object) {
            return false;
        }

        {
            std::shared_lock lock(g_shadowGeometryClassLock);
            auto it = g_shadowObjectHasDynamicBSXFlags.find(object);
            if (it != g_shadowObjectHasDynamicBSXFlags.end()) {
                return it->second;
            }
        }

        const bool hasDynamicFlags = ComputeDynamicBSXFlags(object);
        if (hasDynamicFlags) {
            std::unique_lock lock(g_shadowGeometryClassLock);
            g_shadowObjectHasDynamicBSXFlags[object] = true;
        }
        return hasDynamicFlags;
    }

    bool HasPackedCombinedExtraInChain(RE::NiAVObject* object)
    {
        for (std::uint32_t depth = 0; object && depth < kMaxShadowParentScan; ++depth, object = object->parent) {
            if (HasPackedCombinedExtra(object)) {
                return true;
            }
        }
        return false;
    }

    bool HasDynamicBSXFlagsInChain(RE::NiAVObject* object)
    {
        for (std::uint32_t depth = 0; object && depth < kMaxShadowParentScan; ++depth, object = object->parent) {
            if (HasDynamicBSXFlags(object)) {
                return true;
            }
        }
        return false;
    }

    bool IsBroadShadowStaticGeometryType(RE::BSGeometry* geometry)
    {
        if (!geometry) {
            return false;
        }

        if (netimmerse_cast<RE::BSTriShape*>(geometry) ||
            netimmerse_cast<RE::BSLODTriShape*>(geometry) ||
            netimmerse_cast<RE::BSLODMultiIndexTriShape*>(geometry) ||
            netimmerse_cast<RE::BSMeshLODTriShape*>(geometry)) {
            return true;
        }

        switch (geometry->type) {
        case 3:   // BSTriShape fallback
        case 5:   // BSMeshLODTriShape fallback
        case 6:   // BSLODMultiIndexTriShape fallback
        case 7:   // BSLODTriShape fallback
            return true;
        default:
            return false;
        }
    }

    bool IsShadowStaticCandidate(RE::BSGeometry* geometry)
    {
        if (!geometry || geometry->skinInstance) {
            return false;
        }

        if (geometry->type == 15 ||
            geometry->IsBSCombinedTriShape() != nullptr ||
            HasPackedCombinedExtraInChain(geometry)) {
            return true;
        }

        return IsBroadShadowStaticGeometryType(geometry);
    }

    bool ComputePrecombineShadowGeometry(RE::BSGeometry* geometry)
    {
        if (!IsShadowStaticCandidate(geometry)) {
            return false;
        }

        // Static shape class is necessary but not sufficient: animated,
        // Havok, articulated, ragdoll, or controller-driven objects must stay
        // in the live dynamic overlay even if their mesh type looks static.
        if (HasDynamicBSXFlagsInChain(geometry)) {
            return false;
        }

        return true;
    }

    bool IsPrecombineShadowGeometry(RE::BSGeometry* geometry)
    {
        if (!geometry) {
            return false;
        }

        {
            std::shared_lock lock(g_shadowGeometryClassLock);
            const auto it = g_shadowGeometryIsPrecombine.find(geometry);
            if (it != g_shadowGeometryIsPrecombine.end()) {
                return it->second;
            }
        }

        const bool isPrecombine = ComputePrecombineShadowGeometry(geometry);
        {
            std::unique_lock lock(g_shadowGeometryClassLock);
            if (g_shadowGeometryIsPrecombine.size() >= kMaxShadowGeometryClassCache) {
                g_shadowGeometryIsPrecombine.clear();
            }
            g_shadowGeometryIsPrecombine[geometry] = isPrecombine;
        }
        return isPrecombine;
    }

    bool TryGetShadowPassClassification(BSRenderPassLayout* pass, bool& isPrecombine)
    {
        if (!pass) {
            return false;
        }

        std::shared_lock lock(g_shadowPassClassLock);
        const auto it = g_shadowPassClassifications.find(pass);
        if (it == g_shadowPassClassifications.end() ||
            it->second.geometry != pass->geometry) {
            return false;
        }

        isPrecombine = (it->second.flags & kShadowPassClassStatic) != 0;
        return true;
    }

    void RememberShadowPassClassification(BSRenderPassLayout* pass, bool isPrecombine)
    {
        if constexpr (!kLegacyFinishLevelShadowSplitEnabled) {
            return;
        }
        if (!pass) {
            return;
        }

        std::unique_lock lock(g_shadowPassClassLock);
        if (g_shadowPassClassifications.size() >= kMaxShadowPassClassCache) {
            g_shadowPassClassifications.clear();
        }
        g_shadowPassClassifications[pass] = {
            pass->geometry,
            static_cast<std::uint8_t>(isPrecombine ? kShadowPassClassStatic : 0)
        };
    }

    bool ClassifyShadowPass(BSRenderPassLayout* pass)
    {
        if (!pass) {
            return false;
        }

        bool isPrecombine = false;
        if (TryGetShadowPassClassification(pass, isPrecombine)) {
            return isPrecombine;
        }

        isPrecombine = IsPrecombineShadowGeometry(pass->geometry);
        RememberShadowPassClassification(pass, isPrecombine);
        return isPrecombine;
    }

    bool ShouldKeepShadowPassForCurrentSplit(BSRenderPassLayout* pass)
    {
        if (!SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON ||
            !ShadowTelemetry::IsShadowCacheActiveForCurrentShadow() ||
            !pass) {
            return true;
        }

        const bool isStaticCaster = ClassifyShadowPass(pass);
        if (ShadowTelemetry::IsShadowCacheStaticBuildPass()) {
            ShadowTelemetry::NoteShadowCacheRenderPassSplit(isStaticCaster, isStaticCaster);
            return isStaticCaster;
        }
        if (ShadowTelemetry::IsShadowCacheDynamicOverlayPass()) {
            const bool keep = !isStaticCaster;
            ShadowTelemetry::NoteShadowCacheRenderPassSplit(keep, isStaticCaster);
            return keep;
        }
        return true;
    }

    constexpr unsigned int kShadowCacheBatchPassGroup = 1;
    constexpr std::size_t kAccumulatorBatchRendererOffset = 0xC8;
    constexpr std::uintptr_t kInvalidBatchNode = 0xFFFFu;
    constexpr std::size_t kBatchRendererGroupStride = 0x18;
    constexpr std::size_t kBatchRendererGroupArrayOffset = 0x08;
    constexpr std::size_t kBatchRendererGroupHeadOffset = 0x2E0;
    constexpr std::size_t kBatchRendererGroupTailOffset = 0x2E4;
    constexpr std::size_t kBatchRendererNodeStride = 0x10;
    constexpr std::size_t kBatchRendererNodeHeadOffset = 0x00;
    constexpr std::size_t kBatchRendererNodeTechniqueOffset = 0x08;
    constexpr std::size_t kBatchRendererNodeNextOffset = 0x0C;
    constexpr std::size_t kCommandBufferWriteCursorOffset = 0x10000;
    constexpr std::size_t kCommandBufferFrameOffset = 0x10010;
    constexpr std::size_t kMaxCommandBufferRecords = 8192;
    bool IsShadowCacheRenderSplitActive() noexcept
    {
        return kLegacyFinishLevelShadowSplitEnabled &&
               ShadowTelemetry::IsShadowCacheActiveForCurrentShadow();
    }

    bool ShouldSplitRenderBatchesCall(unsigned int passGroupIdx, unsigned int filter) noexcept
    {
        if (!IsShadowCacheRenderSplitActive()) {
            return false;
        }

        // FinishAccumulating_ShadowMapOrMask consumes accumulator group 1, then
        // RenderGeometryGroup(group=9). The latter can render a nested batch
        // renderer across groups 0..12 with filter=9.
        return passGroupIdx == kShadowCacheBatchPassGroup || filter == 9;
    }

    std::uintptr_t GetRenderBatchNodeAddress(void* batchRenderer, int group, unsigned int subIdx)
    {
        if (!batchRenderer || group < 0 || group >= 32 || subIdx >= 0xFFFFu) {
            return 0;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(batchRenderer);
        const auto nodeArray = *reinterpret_cast<std::uintptr_t*>(
            base + kBatchRendererGroupStride * static_cast<unsigned int>(group) +
            kBatchRendererGroupArrayOffset);
        if (!nodeArray) {
            return 0;
        }

        return nodeArray + kBatchRendererNodeStride * subIdx;
    }

    BSRenderPassLayout* GetRenderBatchNodeHeadRaw(std::uintptr_t node)
    {
        return node ? *reinterpret_cast<BSRenderPassLayout**>(node + kBatchRendererNodeHeadOffset) : nullptr;
    }
    BSRenderPassLayout* GetRenderBatchNodeHead(void* batchRenderer, int group, unsigned int subIdx)
    {
        return GetRenderBatchNodeHeadRaw(GetRenderBatchNodeAddress(batchRenderer, group, subIdx));
    }

    void SetRenderBatchNodeHeadRaw(std::uintptr_t node, BSRenderPassLayout* head)
    {
        if (node) {
            *reinterpret_cast<BSRenderPassLayout**>(node + kBatchRendererNodeHeadOffset) = head;
        }
    }

    std::uint32_t GetRenderBatchNodeTechniqueRaw(std::uintptr_t node)
    {
        return node ? *reinterpret_cast<std::uint32_t*>(node + kBatchRendererNodeTechniqueOffset) : 0;
    }

    void InvalidateCommandBufferFrame(void* cbData)
    {
        if (cbData) {
            *reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(cbData) + kCommandBufferFrameOffset) = 0xFFFFFFFFu;
        }
    }

    class ScopedStaticRenderBatchesRestore
    {
    public:
        ScopedStaticRenderBatchesRestore(void* batchRenderer, unsigned int group) :
            batchRenderer_(reinterpret_cast<std::uintptr_t>(batchRenderer)),
            group_(group)
        {
            if (!batchRenderer_ ||
                group_ != kShadowCacheBatchPassGroup ||
                !ShadowTelemetry::IsShadowCacheStaticBuildPass()) {
                return;
            }

            const auto nodeArray = *reinterpret_cast<std::uintptr_t*>(
                batchRenderer_ + kBatchRendererGroupStride * group_ + kBatchRendererGroupArrayOffset);
            if (!nodeArray) {
                return;
            }

            originalHead_ = *reinterpret_cast<std::uint32_t*>(
                batchRenderer_ + kBatchRendererGroupHeadOffset + group_ * sizeof(std::uint64_t));
            originalTail_ = *reinterpret_cast<std::uint32_t*>(
                batchRenderer_ + kBatchRendererGroupTailOffset + group_ * sizeof(std::uint64_t));

            std::uint32_t nodeIndex = originalHead_;
            for (std::size_t count = 0; nodeIndex != kInvalidBatchNode && count < 65536; ++count) {
                const std::uintptr_t node = nodeArray + kBatchRendererNodeStride * nodeIndex;
                nodes_.push_back({ node, GetRenderBatchNodeHeadRaw(node) });
                nodeIndex = *reinterpret_cast<std::uint16_t*>(node + kBatchRendererNodeNextOffset);
            }

            active_ = true;
        }

        ~ScopedStaticRenderBatchesRestore()
        {
            if (!active_) {
                return;
            }

            *reinterpret_cast<std::uint32_t*>(
                batchRenderer_ + kBatchRendererGroupHeadOffset + group_ * sizeof(std::uint64_t)) = originalHead_;
            *reinterpret_cast<std::uint32_t*>(
                batchRenderer_ + kBatchRendererGroupTailOffset + group_ * sizeof(std::uint64_t)) = originalTail_;

            for (const auto& node : nodes_) {
                SetRenderBatchNodeHeadRaw(node.address, node.head);
            }
        }

        ScopedStaticRenderBatchesRestore(const ScopedStaticRenderBatchesRestore&) = delete;
        ScopedStaticRenderBatchesRestore& operator=(const ScopedStaticRenderBatchesRestore&) = delete;

    private:
        struct NodeHead
        {
            std::uintptr_t address = 0;
            BSRenderPassLayout* head = nullptr;
        };

        std::uintptr_t batchRenderer_ = 0;
        unsigned int group_ = 0;
        std::uint32_t originalHead_ = 0xFFFFu;
        std::uint32_t originalTail_ = 0xFFFFu;
        bool active_ = false;
        std::vector<NodeHead> nodes_;
    };

    enum class ShadowSplitPhase : std::uint8_t
    {
        None,
        StaticBuild,
        DynamicOverlay
    };

    constexpr std::size_t kGeometryGroupPersistentHeadOffset = 0x08;
    constexpr std::size_t kGeometryGroupFlagsOffset = 0x20;

    struct alignas(16) CommandBufferPassesDataSidecar
    {
        std::array<void*, kMaxCommandBufferRecords> records{};
        void** writeCursor = records.data();
        std::uint64_t pad10008 = 0;
        std::uint32_t frame = 0xFFFFFFFFu;
        std::uint32_t pad10014 = 0;
    };
    static_assert(offsetof(CommandBufferPassesDataSidecar, records) == 0x0);
    static_assert(offsetof(CommandBufferPassesDataSidecar, writeCursor) == kCommandBufferWriteCursorOffset);
    static_assert(offsetof(CommandBufferPassesDataSidecar, frame) == kCommandBufferFrameOffset);

    struct ShadowSplitChain
    {
        BSRenderPassLayout* head = nullptr;
        BSRenderPassLayout* tail = nullptr;
        std::vector<BSRenderPassLayout*> passes;
        std::vector<void*> commandBuffers;
    };

    struct ShadowSplitChains
    {
        ShadowSplitChain staticChain;
        ShadowSplitChain dynamicChain;
    };

    struct ActiveCommandBufferSelection
    {
        std::uintptr_t node = 0;
        BSRenderPassLayout* head = nullptr;
        std::uint32_t techniqueID = 0;
        std::vector<void*> commandBuffers;
    };

    struct CommandBufferSidecarCacheEntry
    {
        void* originalCbData = nullptr;
        std::uintptr_t node = 0;
        ShadowSplitPhase phase = ShadowSplitPhase::None;
        std::unique_ptr<CommandBufferPassesDataSidecar> data;
        std::vector<void*> selectedCommandBuffers;
        void** sourceWriteCursor = nullptr;
        std::uint32_t sourceFrame = 0xFFFFFFFFu;
    };

    struct PassGroupState
    {
        std::uint32_t head = 0xFFFFu;
        std::uint32_t tail = 0xFFFFu;
    };

    struct NodeState
    {
        std::uintptr_t address = 0;
        BSRenderPassLayout* head = nullptr;
        std::uint16_t next = 0xFFFFu;
    };

    struct PassLinkState
    {
        BSRenderPassLayout* pass = nullptr;
        BSRenderPassLayout* next = nullptr;
    };

    thread_local std::uint32_t tl_finishLevelShadowSplitDepth = 0;
    thread_local std::vector<ActiveCommandBufferSelection> tl_shadowCommandSelections;
    thread_local std::vector<CommandBufferSidecarCacheEntry> tl_commandBufferSidecars;
    constexpr std::size_t kMaxShadowCommandBufferSidecars = 256;

    ShadowSplitPhase CurrentShadowSplitPhase() noexcept
    {
        if (ShadowTelemetry::IsShadowCacheStaticBuildPass()) {
            return ShadowSplitPhase::StaticBuild;
        }
        if (ShadowTelemetry::IsShadowCacheDynamicOverlayPass()) {
            return ShadowSplitPhase::DynamicOverlay;
        }
        return ShadowSplitPhase::None;
    }

    bool IsFinishLevelShadowSplitActive() noexcept
    {
        return tl_finishLevelShadowSplitDepth != 0;
    }

    class ScopedFinishLevelShadowSplit
    {
    public:
        ScopedFinishLevelShadowSplit()
        {
            ++tl_finishLevelShadowSplitDepth;
        }

        ~ScopedFinishLevelShadowSplit()
        {
            --tl_finishLevelShadowSplitDepth;
        }

        ScopedFinishLevelShadowSplit(const ScopedFinishLevelShadowSplit&) = delete;
        ScopedFinishLevelShadowSplit& operator=(const ScopedFinishLevelShadowSplit&) = delete;
    };

    void AddPassToShadowSplitChain(ShadowSplitChain& chain, BSRenderPassLayout* pass)
    {
        if (!pass) {
            return;
        }

        if (!chain.head) {
            chain.head = pass;
        }
        chain.tail = pass;
        chain.passes.push_back(pass);
        auto* commandBufferRecord = reinterpret_cast<void*>(pass->next);
        if (!commandBufferRecord) {
            commandBufferRecord = pass->commandBuffer;
        }
        if (commandBufferRecord) {
            chain.commandBuffers.push_back(commandBufferRecord);
        }
    }

    ShadowSplitChains BuildShadowSplitChains(BSRenderPassLayout* head)
    {
        ShadowSplitChains chains{};
        const auto phase = CurrentShadowSplitPhase();
        if (phase == ShadowSplitPhase::None || !head) {
            return chains;
        }

        std::uint32_t count = 0;
        for (auto* pass = head; pass && count < 65536; pass = pass->passGroupNext, ++count) {
            const bool isStaticCaster = ClassifyShadowPass(pass);
            if (phase == ShadowSplitPhase::StaticBuild) {
                ShadowTelemetry::NoteShadowCacheRenderPassSplit(isStaticCaster, isStaticCaster);
            } else if (phase == ShadowSplitPhase::DynamicOverlay) {
                ShadowTelemetry::NoteShadowCacheRenderPassSplit(!isStaticCaster, isStaticCaster);
            }

            if (isStaticCaster) {
                AddPassToShadowSplitChain(chains.staticChain, pass);
            } else {
                AddPassToShadowSplitChain(chains.dynamicChain, pass);
            }
        }
        return chains;
    }

    const ShadowSplitChain& SelectShadowSplitChain(const ShadowSplitChains& chains) noexcept
    {
        return ShadowTelemetry::IsShadowCacheStaticBuildPass() ? chains.staticChain : chains.dynamicChain;
    }

    void ExposeShadowSplitChain(const ShadowSplitChain& chain, std::vector<PassLinkState>& originalLinks)
    {
        for (std::size_t i = 0; i < chain.passes.size(); ++i) {
            auto* pass = chain.passes[i];
            if (!pass) {
                continue;
            }
            originalLinks.push_back({ pass, pass->passGroupNext });
            pass->passGroupNext = (i + 1 < chain.passes.size()) ? chain.passes[i + 1] : nullptr;
        }
    }

    void RestorePassLinks(std::vector<PassLinkState>& originalLinks)
    {
        for (const auto& link : originalLinks) {
            if (link.pass) {
                link.pass->passGroupNext = link.next;
            }
        }
        originalLinks.clear();
    }

    std::uintptr_t GetBatchRendererNodeArray(std::uintptr_t batchRenderer, unsigned int group)
    {
        if (!batchRenderer || group >= 32) {
            return 0;
        }
        return *reinterpret_cast<std::uintptr_t*>(
            batchRenderer + kBatchRendererGroupStride * group + kBatchRendererGroupArrayOffset);
    }

    PassGroupState ReadBatchGroupState(std::uintptr_t batchRenderer, unsigned int group)
    {
        PassGroupState state{};
        if (!batchRenderer || group >= 32) {
            return state;
        }
        state.head = *reinterpret_cast<std::uint32_t*>(
            batchRenderer + kBatchRendererGroupHeadOffset + group * sizeof(std::uint64_t));
        state.tail = *reinterpret_cast<std::uint32_t*>(
            batchRenderer + kBatchRendererGroupTailOffset + group * sizeof(std::uint64_t));
        return state;
    }

    void WriteBatchGroupState(std::uintptr_t batchRenderer, unsigned int group, PassGroupState state)
    {
        if (!batchRenderer || group >= 32) {
            return;
        }
        *reinterpret_cast<std::uint32_t*>(
            batchRenderer + kBatchRendererGroupHeadOffset + group * sizeof(std::uint64_t)) = state.head;
        *reinterpret_cast<std::uint32_t*>(
            batchRenderer + kBatchRendererGroupTailOffset + group * sizeof(std::uint64_t)) = state.tail;
    }

    std::size_t GetCommandBufferRecordCount(void* cbData)
    {
        if (!cbData) {
            return 0;
        }

        auto** records = static_cast<void**>(cbData);
        auto** cursor = *reinterpret_cast<void***>(
            static_cast<std::byte*>(cbData) + kCommandBufferWriteCursorOffset);
        const auto recordsAddr = reinterpret_cast<std::uintptr_t>(records);
        const auto cursorAddr = reinterpret_cast<std::uintptr_t>(cursor);
        const auto maxCursorAddr = recordsAddr + kMaxCommandBufferRecords * sizeof(void*);
        if (cursorAddr >= recordsAddr &&
            cursorAddr <= maxCursorAddr &&
            ((cursorAddr - recordsAddr) % sizeof(void*)) == 0) {
            return static_cast<std::size_t>((cursorAddr - recordsAddr) / sizeof(void*));
        }

        std::size_t count = 0;
        while (count < kMaxCommandBufferRecords && records[count]) {
            ++count;
        }
        return count;
    }

    bool ContainsPointer(const std::vector<void*>& values, void* needle)
    {
        return std::find(values.begin(), values.end(), needle) != values.end();
    }

    CommandBufferSidecarCacheEntry& GetCommandBufferSidecarEntry(
        void* cbData,
        std::uintptr_t node,
        ShadowSplitPhase phase)
    {
        for (auto& entry : tl_commandBufferSidecars) {
            if (entry.originalCbData == cbData &&
                entry.node == node &&
                entry.phase == phase) {
                return entry;
            }
        }

        if (tl_commandBufferSidecars.size() >= kMaxShadowCommandBufferSidecars) {
            tl_commandBufferSidecars.clear();
        }

        auto& entry = tl_commandBufferSidecars.emplace_back();
        entry.originalCbData = cbData;
        entry.node = node;
        entry.phase = phase;
        entry.data = std::make_unique<CommandBufferPassesDataSidecar>();
        return entry;
    }

    ActiveCommandBufferSelection* FindActiveCommandBufferSelection(std::uintptr_t node)
    {
        if (!node) {
            return nullptr;
        }

        for (auto it = tl_shadowCommandSelections.rbegin(); it != tl_shadowCommandSelections.rend(); ++it) {
            if (it->node == node) {
                return std::addressof(*it);
            }
        }
        return nullptr;
    }

    std::vector<void*> CollectCommandBufferRecords(BSRenderPassLayout* head)
    {
        std::vector<void*> records;
        for (auto* pass = head; pass && records.size() < 65536; pass = pass->passGroupNext) {
            auto* commandBufferRecord = reinterpret_cast<void*>(pass->next);
            if (!commandBufferRecord) {
                commandBufferRecord = pass->commandBuffer;
            }
            if (commandBufferRecord) {
                records.push_back(commandBufferRecord);
            }
        }
        return records;
    }

    void* GetPassCommandBufferRecord(BSRenderPassLayout* pass) noexcept
    {
        if (!pass) {
            return nullptr;
        }

        auto* commandBufferRecord = reinterpret_cast<void*>(pass->next);
        if (!commandBufferRecord) {
            commandBufferRecord = pass->commandBuffer;
        }
        return commandBufferRecord;
    }

    void* PrepareShadowCommandBufferSidecar(
        void* cbData,
        const ActiveCommandBufferSelection& selection,
        ShadowSplitPhase phase)
    {
        if (!cbData || selection.commandBuffers.empty() || phase == ShadowSplitPhase::None) {
            return nullptr;
        }

        auto& entry = GetCommandBufferSidecarEntry(cbData, selection.node, phase);
        if (!entry.data) {
            entry.data = std::make_unique<CommandBufferPassesDataSidecar>();
        }

        auto** sourceRecords = static_cast<void**>(cbData);
        auto** sourceCursor = *reinterpret_cast<void***>(
            static_cast<std::byte*>(cbData) + kCommandBufferWriteCursorOffset);
        const auto sourceFrame = *reinterpret_cast<std::uint32_t*>(
            static_cast<std::byte*>(cbData) + kCommandBufferFrameOffset);
        const bool needsRebuild =
            entry.sourceWriteCursor != sourceCursor ||
            entry.sourceFrame != sourceFrame ||
            entry.selectedCommandBuffers != selection.commandBuffers;

        if (needsRebuild) {
            std::fill(entry.data->records.begin(), entry.data->records.end(), nullptr);
            const std::size_t sourceCount = GetCommandBufferRecordCount(cbData);
            std::size_t keptCount = 0;
            std::vector<void*> keptRecords;
            keptRecords.reserve(selection.commandBuffers.size());

            for (std::size_t i = 0; i < sourceCount && keptCount < kMaxCommandBufferRecords; ++i) {
                void* record = sourceRecords[i];
                if (record && ContainsPointer(selection.commandBuffers, record)) {
                    entry.data->records[keptCount++] = record;
                    keptRecords.push_back(record);
                }
            }

            for (void* commandBuffer : selection.commandBuffers) {
                if (commandBuffer && !ContainsPointer(keptRecords, commandBuffer)) {
                    return nullptr;
                }
            }
            if (keptCount == 0) {
                return nullptr;
            }

            entry.data->writeCursor = entry.data->records.data() + keptCount;
            if (keptCount < kMaxCommandBufferRecords) {
                entry.data->records[keptCount] = nullptr;
            }
            entry.selectedCommandBuffers = selection.commandBuffers;
            entry.sourceWriteCursor = sourceCursor;
            entry.sourceFrame = sourceFrame;
        }

        entry.data->frame = sourceFrame;
        return entry.data.get();
    }

    class ScopedShadowBatchGroupExposure
    {
    public:
        ScopedShadowBatchGroupExposure(void* batchRenderer, unsigned int group) :
            batchRenderer_(reinterpret_cast<std::uintptr_t>(batchRenderer)),
            group_(group)
        {
            if (!IsShadowCacheRenderSplitActive() ||
                !batchRenderer_) {
                return;
            }

            nodeArray_ = GetBatchRendererNodeArray(batchRenderer_, group_);
            if (!nodeArray_) {
                return;
            }

            active_ = true;
            originalGroup_ = ReadBatchGroupState(batchRenderer_, group_);
            const bool clearAfterRender = *reinterpret_cast<std::uint8_t*>(batchRenderer_ + 0x350) != 0;
            restore_ = ShadowTelemetry::IsShadowCacheStaticBuildPass() || !clearAfterRender;
            std::uint32_t filteredHead = 0xFFFFu;
            std::uint32_t filteredTail = 0xFFFFu;

            std::uint32_t nodeIndex = originalGroup_.head;
            for (std::size_t count = 0; nodeIndex != kInvalidBatchNode && count < 65536; ++count) {
                const std::uintptr_t node = nodeArray_ + kBatchRendererNodeStride * nodeIndex;
                const auto originalHead = GetRenderBatchNodeHeadRaw(node);
                const auto originalNext = *reinterpret_cast<std::uint16_t*>(node + kBatchRendererNodeNextOffset);
                originalNodes_.push_back({ node, originalHead, originalNext });

                const auto chains = BuildShadowSplitChains(originalHead);
                const auto& selected = SelectShadowSplitChain(chains);
                ExposeShadowSplitChain(selected, originalPassLinks_);
                SetRenderBatchNodeHeadRaw(node, selected.head);

                if (selected.head) {
                    if (filteredHead == 0xFFFFu) {
                        filteredHead = nodeIndex;
                    } else if (filteredTail != 0xFFFFu) {
                        const std::uintptr_t tailNode = nodeArray_ + kBatchRendererNodeStride * filteredTail;
                        *reinterpret_cast<std::uint16_t*>(tailNode + kBatchRendererNodeNextOffset) =
                            static_cast<std::uint16_t>(nodeIndex);
                    }
                    filteredTail = nodeIndex;
                    *reinterpret_cast<std::uint16_t*>(node + kBatchRendererNodeNextOffset) = 0xFFFFu;

                    tl_shadowCommandSelections.push_back({
                        node,
                        selected.head,
                        GetRenderBatchNodeTechniqueRaw(node),
                        selected.commandBuffers
                    });
                    ++commandSelectionCount_;
                } else if (!restore_ && clearAfterRender) {
                    SetRenderBatchNodeHeadRaw(node, nullptr);
                }

                nodeIndex = originalNext;
            }

            WriteBatchGroupState(batchRenderer_, group_, { filteredHead, filteredTail });
            empty_ = filteredHead == 0xFFFFu;
        }

        ~ScopedShadowBatchGroupExposure()
        {
            if (!active_) {
                return;
            }

            RestorePassLinks(originalPassLinks_);
            if (commandSelectionCount_ <= tl_shadowCommandSelections.size()) {
                tl_shadowCommandSelections.resize(tl_shadowCommandSelections.size() - commandSelectionCount_);
            } else {
                tl_shadowCommandSelections.clear();
            }

            if (!restore_) {
                for (const auto& node : originalNodes_) {
                    *reinterpret_cast<std::uint16_t*>(node.address + kBatchRendererNodeNextOffset) = node.next;
                }
                return;
            }

            WriteBatchGroupState(batchRenderer_, group_, originalGroup_);
            for (const auto& node : originalNodes_) {
                SetRenderBatchNodeHeadRaw(node.address, node.head);
                *reinterpret_cast<std::uint16_t*>(node.address + kBatchRendererNodeNextOffset) = node.next;
            }
        }

        bool Active() const noexcept { return active_; }
        bool Empty() const noexcept { return active_ && empty_; }

        ScopedShadowBatchGroupExposure(const ScopedShadowBatchGroupExposure&) = delete;
        ScopedShadowBatchGroupExposure& operator=(const ScopedShadowBatchGroupExposure&) = delete;

    private:
        std::uintptr_t batchRenderer_ = 0;
        unsigned int group_ = 0;
        std::uintptr_t nodeArray_ = 0;
        PassGroupState originalGroup_{};
        std::vector<NodeState> originalNodes_;
        std::vector<PassLinkState> originalPassLinks_;
        std::size_t commandSelectionCount_ = 0;
        bool active_ = false;
        bool restore_ = false;
        bool empty_ = false;
    };

    class ScopedPersistentShadowListExposure
    {
    public:
        explicit ScopedPersistentShadowListExposure(void* persistentPassList) :
            list_(static_cast<BSRenderPassLayout**>(persistentPassList))
        {
            if (!list_ || !IsShadowCacheRenderSplitActive()) {
                return;
            }

            active_ = true;
            restore_ = ShadowTelemetry::IsShadowCacheStaticBuildPass();
            originalHead_ = list_[0];
            originalTail_ = list_[1];

            const auto chains = BuildShadowSplitChains(originalHead_);
            const auto& selected = SelectShadowSplitChain(chains);
            ExposeShadowSplitChain(selected, originalPassLinks_);
            list_[0] = selected.head;
            list_[1] = selected.tail;
        }

        ~ScopedPersistentShadowListExposure()
        {
            if (!active_) {
                return;
            }

            RestorePassLinks(originalPassLinks_);
            if (restore_ && list_) {
                list_[0] = originalHead_;
                list_[1] = originalTail_;
            }
        }

        bool Active() const noexcept { return active_; }

        ScopedPersistentShadowListExposure(const ScopedPersistentShadowListExposure&) = delete;
        ScopedPersistentShadowListExposure& operator=(const ScopedPersistentShadowListExposure&) = delete;

    private:
        BSRenderPassLayout** list_ = nullptr;
        BSRenderPassLayout* originalHead_ = nullptr;
        BSRenderPassLayout* originalTail_ = nullptr;
        std::vector<PassLinkState> originalPassLinks_;
        bool active_ = false;
        bool restore_ = false;
    };

    class ScopedShadowPassChainFilter
    {
    public:
        explicit ScopedShadowPassChainFilter(BSRenderPassLayout* head)
        {
            if (IsFinishLevelShadowSplitActive() ||
                !IsShadowCacheRenderSplitActive()) {
                filteredHead_ = head;
                return;
            }

            BSRenderPassLayout* tail = nullptr;
            for (auto* pass = head; pass && originalLinks_.size() < 65536; pass = pass->passGroupNext) {
                originalLinks_.push_back({ pass, pass->passGroupNext });
                if (!ShouldKeepShadowPassForCurrentSplit(pass)) {
                    continue;
                }

                if (!filteredHead_) {
                    filteredHead_ = pass;
                }
                if (tail) {
                    tail->passGroupNext = pass;
                }
                tail = pass;
            }

            if (tail) {
                tail->passGroupNext = nullptr;
            }
        }

        ~ScopedShadowPassChainFilter()
        {
            for (const auto& link : originalLinks_) {
                if (link.pass) {
                    link.pass->passGroupNext = link.next;
                }
            }
        }

        BSRenderPassLayout* Head() const noexcept
        {
            return filteredHead_;
        }

        ScopedShadowPassChainFilter(const ScopedShadowPassChainFilter&) = delete;
        ScopedShadowPassChainFilter& operator=(const ScopedShadowPassChainFilter&) = delete;

    private:
        struct Link
        {
            BSRenderPassLayout* pass = nullptr;
            BSRenderPassLayout* next = nullptr;
        };

        BSRenderPassLayout* filteredHead_ = nullptr;
        std::vector<Link> originalLinks_;
    };

    class ScopedCommandBufferPassesFilter
    {
    public:
        ScopedCommandBufferPassesFilter(void* batchRenderer, int group, void* cbData, unsigned int subIdx) :
            cbData_(cbData),
            records_(static_cast<void**>(cbData)),
            node_(GetRenderBatchNodeAddress(batchRenderer, group, subIdx))
        {
            if (IsFinishLevelShadowSplitActive() ||
                !IsShadowCacheRenderSplitActive() ||
                group != static_cast<int>(kShadowCacheBatchPassGroup) ||
                !cbData_ ||
                !records_ ||
                !node_) {
                return;
            }

            active_ = true;
            restoreFrame_ = ShadowTelemetry::IsShadowCacheStaticBuildPass();
            originalFrame_ = *reinterpret_cast<std::uint32_t*>(
                static_cast<std::byte*>(cbData_) + kCommandBufferFrameOffset);
            originalNodeHead_ = GetRenderBatchNodeHeadRaw(node_);
            techniqueID_ = GetRenderBatchNodeTechniqueRaw(node_);
            writeCursorSlot_ = reinterpret_cast<void***>(
                static_cast<std::byte*>(cbData_) + kCommandBufferWriteCursorOffset);
            originalWriteCursor_ = *writeCursorSlot_;

            const std::size_t recordCount = GetOriginalRecordCount();
            originalRecords_.assign(records_, records_ + recordCount);
            if (recordCount < kMaxCommandBufferRecords) {
                terminatorSlot_ = records_ + recordCount;
                originalTerminator_ = *terminatorSlot_;
            }

            BSRenderPassLayout* filteredHead = nullptr;
            BSRenderPassLayout* filteredTail = nullptr;
            std::vector<void*> keptCommandBuffers;
            for (auto* pass = originalNodeHead_; pass && originalLinks_.size() < 65536; pass = pass->passGroupNext) {
                originalLinks_.push_back({ pass, pass->passGroupNext });
                if (!ShouldKeepShadowPassForCurrentSplit(pass)) {
                    continue;
                }

                if (!filteredHead) {
                    filteredHead = pass;
                }
                if (filteredTail) {
                    filteredTail->passGroupNext = pass;
                }
                filteredTail = pass;
                if (pass->commandBuffer) {
                    keptCommandBuffers.push_back(pass->commandBuffer);
                }
            }

            if (filteredTail) {
                filteredTail->passGroupNext = nullptr;
            }
            filteredHead_ = filteredHead;
            SetRenderBatchNodeHeadRaw(node_, filteredHead_);

            for (void* record : originalRecords_) {
                if (Contains(keptCommandBuffers, record)) {
                    records_[keptRecordCount_++] = record;
                }
            }
            *writeCursorSlot_ = records_ + keptRecordCount_;
            if (keptRecordCount_ < kMaxCommandBufferRecords) {
                records_[keptRecordCount_] = nullptr;
            }

            for (void* record : keptCommandBuffers) {
                if (!Contains(originalRecords_, record)) {
                    needsImmediateFallback_ = true;
                    break;
                }
            }
        }

        ~ScopedCommandBufferPassesFilter()
        {
            if (!active_) {
                return;
            }

            for (const auto& link : originalLinks_) {
                if (link.pass) {
                    link.pass->passGroupNext = link.next;
                }
            }
            SetRenderBatchNodeHeadRaw(node_, originalNodeHead_);

            for (std::size_t i = 0; i < originalRecords_.size(); ++i) {
                records_[i] = originalRecords_[i];
            }
            if (terminatorSlot_) {
                *terminatorSlot_ = originalTerminator_;
            }
            if (writeCursorSlot_) {
                *writeCursorSlot_ = originalWriteCursor_;
            }
            if (restoreFrame_) {
                *reinterpret_cast<std::uint32_t*>(
                    static_cast<std::byte*>(cbData_) + kCommandBufferFrameOffset) = originalFrame_;
            }
        }

        bool Active() const noexcept { return active_; }
        BSRenderPassLayout* Head() const noexcept { return filteredHead_; }
        std::uint32_t TechniqueID() const noexcept { return techniqueID_; }
        bool NeedsImmediateFallback() const noexcept
        {
            return active_ && filteredHead_ && (keptRecordCount_ == 0 || needsImmediateFallback_);
        }

        ScopedCommandBufferPassesFilter(const ScopedCommandBufferPassesFilter&) = delete;
        ScopedCommandBufferPassesFilter& operator=(const ScopedCommandBufferPassesFilter&) = delete;

    private:
        static bool Contains(const std::vector<void*>& records, void* needle)
        {
            return std::find(records.begin(), records.end(), needle) != records.end();
        }

        std::size_t GetOriginalRecordCount() const
        {
            const auto recordsAddr = reinterpret_cast<std::uintptr_t>(records_);
            const auto cursorAddr = reinterpret_cast<std::uintptr_t>(originalWriteCursor_);
            const auto maxCursorAddr = recordsAddr + kMaxCommandBufferRecords * sizeof(void*);
            if (cursorAddr >= recordsAddr && cursorAddr <= maxCursorAddr &&
                ((cursorAddr - recordsAddr) % sizeof(void*)) == 0) {
                return static_cast<std::size_t>((cursorAddr - recordsAddr) / sizeof(void*));
            }

            std::size_t count = 0;
            while (count < kMaxCommandBufferRecords && records_[count]) {
                ++count;
            }
            return count;
        }

        struct Link
        {
            BSRenderPassLayout* pass = nullptr;
            BSRenderPassLayout* next = nullptr;
        };

        void* cbData_ = nullptr;
        void** records_ = nullptr;
        void*** writeCursorSlot_ = nullptr;
        void** originalWriteCursor_ = nullptr;
        void** terminatorSlot_ = nullptr;
        void* originalTerminator_ = nullptr;
        std::uintptr_t node_ = 0;
        BSRenderPassLayout* originalNodeHead_ = nullptr;
        BSRenderPassLayout* filteredHead_ = nullptr;
        std::uint32_t techniqueID_ = 0;
        std::uint32_t originalFrame_ = 0xFFFFFFFFu;
        std::size_t keptRecordCount_ = 0;
        bool active_ = false;
        bool restoreFrame_ = false;
        bool needsImmediateFallback_ = false;
        std::vector<void*> originalRecords_;
        std::vector<Link> originalLinks_;
    };

    constexpr bool kDeferredPrePassStaticCommandBufferFilterEnabled = false;

    class ScopedDeferredPrePassStaticCommandBufferFilter
    {
    public:
        ScopedDeferredPrePassStaticCommandBufferFilter(void* batchRenderer, int group, void* cbData, unsigned int subIdx) :
            cbData_(cbData),
            records_(static_cast<void**>(cbData)),
            node_(GetRenderBatchNodeAddress(batchRenderer, group, subIdx))
        {
            if (!kDeferredPrePassStaticCommandBufferFilterEnabled ||
                !PhaseTelemetry::IsInDeferredPrePass() ||
                group != 4 ||
                !cbData_ ||
                !records_ ||
                !node_) {
                return;
            }

            originalNodeHead_ = GetRenderBatchNodeHeadRaw(node_);
            if (!originalNodeHead_) {
                return;
            }

            writeCursorSlot_ = reinterpret_cast<void***>(
                static_cast<std::byte*>(cbData_) + kCommandBufferWriteCursorOffset);
            originalWriteCursor_ = *writeCursorSlot_;

            const std::size_t recordCount = GetOriginalRecordCount();
            originalRecords_.assign(records_, records_ + recordCount);
            if (recordCount < kMaxCommandBufferRecords) {
                terminatorSlot_ = records_ + recordCount;
                originalTerminator_ = *terminatorSlot_;
            }

            BSRenderPassLayout* filteredHead = nullptr;
            BSRenderPassLayout* filteredTail = nullptr;
            std::vector<void*> keptCommandBuffers;
            std::size_t staticPasses = 0;
            std::size_t dynamicPasses = 0;

            for (auto* pass = originalNodeHead_; pass && originalLinks_.size() < 65536; pass = pass->passGroupNext) {
                originalLinks_.push_back({ pass, pass->passGroupNext });
                if (IsPrecombineShadowGeometry(pass->geometry)) {
                    ++staticPasses;
                    continue;
                }

                ++dynamicPasses;
                if (!filteredHead) {
                    filteredHead = pass;
                }
                if (filteredTail) {
                    filteredTail->passGroupNext = pass;
                }
                filteredTail = pass;

                if (auto* record = GetPassCommandBufferRecord(pass)) {
                    keptCommandBuffers.push_back(record);
                }
            }

            if (staticPasses == 0) {
                RestorePassLinks();
                return;
            }

            active_ = true;
            staticPasses_ = staticPasses;
            dynamicPasses_ = dynamicPasses;
            techniqueID_ = GetRenderBatchNodeTechniqueRaw(node_);

            if (filteredTail) {
                filteredTail->passGroupNext = nullptr;
            }
            filteredHead_ = filteredHead;
            SetRenderBatchNodeHeadRaw(node_, filteredHead_);

            for (void* record : originalRecords_) {
                if (Contains(keptCommandBuffers, record)) {
                    records_[keptRecordCount_++] = record;
                }
            }
            *writeCursorSlot_ = records_ + keptRecordCount_;
            if (keptRecordCount_ < kMaxCommandBufferRecords) {
                records_[keptRecordCount_] = nullptr;
            }

            for (void* record : keptCommandBuffers) {
                if (!Contains(originalRecords_, record)) {
                    needsImmediateFallback_ = true;
                    break;
                }
            }

            static std::atomic_bool logged{ false };
            if (!logged.exchange(true, std::memory_order_relaxed)) {
                REX::WARN(
                    "DeferredPrePass experiment: filtering group 4 static command-buffer records "
                    "(first node staticPasses={} dynamicPasses={} records={} kept={})",
                    staticPasses_,
                    dynamicPasses_,
                    originalRecords_.size(),
                    keptRecordCount_);
            }
        }

        ~ScopedDeferredPrePassStaticCommandBufferFilter()
        {
            if (!active_) {
                return;
            }

            RestorePassLinks();
            SetRenderBatchNodeHeadRaw(node_, originalNodeHead_);

            for (std::size_t i = 0; i < originalRecords_.size(); ++i) {
                records_[i] = originalRecords_[i];
            }
            if (terminatorSlot_) {
                *terminatorSlot_ = originalTerminator_;
            }
            if (writeCursorSlot_) {
                *writeCursorSlot_ = originalWriteCursor_;
            }
        }

        bool Active() const noexcept { return active_; }
        BSRenderPassLayout* Head() const noexcept { return filteredHead_; }
        std::uint32_t TechniqueID() const noexcept { return techniqueID_; }
        bool NeedsImmediateFallback() const noexcept
        {
            return active_ && filteredHead_ && (keptRecordCount_ == 0 || needsImmediateFallback_);
        }

        ScopedDeferredPrePassStaticCommandBufferFilter(const ScopedDeferredPrePassStaticCommandBufferFilter&) = delete;
        ScopedDeferredPrePassStaticCommandBufferFilter& operator=(const ScopedDeferredPrePassStaticCommandBufferFilter&) = delete;

    private:
        struct Link
        {
            BSRenderPassLayout* pass = nullptr;
            BSRenderPassLayout* next = nullptr;
        };

        static bool Contains(const std::vector<void*>& records, void* needle)
        {
            return std::find(records.begin(), records.end(), needle) != records.end();
        }

        std::size_t GetOriginalRecordCount() const
        {
            const auto recordsAddr = reinterpret_cast<std::uintptr_t>(records_);
            const auto cursorAddr = reinterpret_cast<std::uintptr_t>(originalWriteCursor_);
            const auto maxCursorAddr = recordsAddr + kMaxCommandBufferRecords * sizeof(void*);
            if (cursorAddr >= recordsAddr && cursorAddr <= maxCursorAddr &&
                ((cursorAddr - recordsAddr) % sizeof(void*)) == 0) {
                return static_cast<std::size_t>((cursorAddr - recordsAddr) / sizeof(void*));
            }

            std::size_t count = 0;
            while (count < kMaxCommandBufferRecords && records_[count]) {
                ++count;
            }
            return count;
        }

        void RestorePassLinks()
        {
            for (const auto& link : originalLinks_) {
                if (link.pass) {
                    link.pass->passGroupNext = link.next;
                }
            }
        }

        void* cbData_ = nullptr;
        void** records_ = nullptr;
        void*** writeCursorSlot_ = nullptr;
        void** originalWriteCursor_ = nullptr;
        void** terminatorSlot_ = nullptr;
        void* originalTerminator_ = nullptr;
        std::uintptr_t node_ = 0;
        BSRenderPassLayout* originalNodeHead_ = nullptr;
        BSRenderPassLayout* filteredHead_ = nullptr;
        std::uint32_t techniqueID_ = 0;
        std::size_t keptRecordCount_ = 0;
        std::size_t staticPasses_ = 0;
        std::size_t dynamicPasses_ = 0;
        bool active_ = false;
        bool needsImmediateFallback_ = false;
        std::vector<void*> originalRecords_;
        std::vector<Link> originalLinks_;
    };

    class ScopedPersistentShadowPassListFilter
    {
    public:
        explicit ScopedPersistentShadowPassListFilter(void* persistentPassList) :
            list_(static_cast<BSRenderPassLayout**>(persistentPassList))
        {
            if (!list_) {
                return;
            }

            originalHead_ = list_[0];
            originalTail_ = list_[1];

            if (IsFinishLevelShadowSplitActive() ||
                !IsShadowCacheRenderSplitActive()) {
                return;
            }

            active_ = true;
            restoreListHead_ = ShadowTelemetry::IsShadowCacheStaticBuildPass();

            BSRenderPassLayout* filteredHead = nullptr;
            BSRenderPassLayout* filteredTail = nullptr;
            for (auto* pass = originalHead_; pass && originalLinks_.size() < 65536; pass = pass->passGroupNext) {
                originalLinks_.push_back({ pass, pass->passGroupNext });
                if (!ShouldKeepShadowPassForCurrentSplit(pass)) {
                    continue;
                }

                if (!filteredHead) {
                    filteredHead = pass;
                }
                if (filteredTail) {
                    filteredTail->passGroupNext = pass;
                }
                filteredTail = pass;
            }

            if (filteredTail) {
                filteredTail->passGroupNext = nullptr;
            }
            list_[0] = filteredHead;
            list_[1] = filteredTail;
        }

        ~ScopedPersistentShadowPassListFilter()
        {
            for (const auto& link : originalLinks_) {
                if (link.pass) {
                    link.pass->passGroupNext = link.next;
                }
            }

            if (active_ && restoreListHead_ && list_) {
                list_[0] = originalHead_;
                list_[1] = originalTail_;
            }
        }

        bool Empty() const noexcept
        {
            return list_ && list_[0] == nullptr;
        }

        ScopedPersistentShadowPassListFilter(const ScopedPersistentShadowPassListFilter&) = delete;
        ScopedPersistentShadowPassListFilter& operator=(const ScopedPersistentShadowPassListFilter&) = delete;

    private:
        struct Link
        {
            BSRenderPassLayout* pass = nullptr;
            BSRenderPassLayout* next = nullptr;
        };

        BSRenderPassLayout** list_ = nullptr;
        BSRenderPassLayout* originalHead_ = nullptr;
        BSRenderPassLayout* originalTail_ = nullptr;
        bool active_ = false;
        bool restoreListHead_ = false;
        std::vector<Link> originalLinks_;
    };

    class ScopedProcessCommandBufferTarget
    {
    public:
        explicit ScopedProcessCommandBufferTarget(const ShadowTelemetry::WorkTarget& target) :
            previous_(tl_processCommandBufferTarget),
            wasActive_(tl_processCommandBufferTargetActive)
        {
            tl_processCommandBufferTarget = target;
            tl_processCommandBufferTargetActive = true;
        }

        ~ScopedProcessCommandBufferTarget()
        {
            tl_processCommandBufferTarget = previous_;
            tl_processCommandBufferTargetActive = wasActive_;
        }

        ScopedProcessCommandBufferTarget(const ScopedProcessCommandBufferTarget&) = delete;
        ScopedProcessCommandBufferTarget& operator=(const ScopedProcessCommandBufferTarget&) = delete;

    private:
        ShadowTelemetry::WorkTarget previous_{};
        bool wasActive_ = false;
    };

    using BSBatchRendererDraw_t = void (*)(BSRenderPassLayout* pass, std::uintptr_t unk2, std::uintptr_t unk3, RE::BSGraphics::DynamicTriShapeDrawData* dynamicDrawData);
    BSBatchRendererDraw_t OriginalBSBatchRendererDraw = nullptr;

    using BSShaderSetupGeometry_t = void (*)(RE::BSShader* shader, BSRenderPassLayout* pass);
    BSShaderSetupGeometry_t OriginalBSLightingShaderSetupGeometry = nullptr;
    BSShaderSetupGeometry_t OriginalBSEffectShaderSetupGeometry = nullptr;

    using BSShaderRestoreGeometry_t = void (*)(RE::BSShader* shader, BSRenderPassLayout* pass);
    BSShaderRestoreGeometry_t OriginalBSLightingShaderRestoreGeometry = nullptr;
    BSShaderRestoreGeometry_t OriginalBSEffectShaderRestoreGeometry = nullptr;

    using BipedAnimAttachSkinnedObject_t = RE::NiAVObject* (*)(RE::BipedAnim* biped, RE::NiNode* destinationRoot, RE::NiNode* sourceRoot, RE::BIPED_OBJECT bipedObject, bool firstPerson);
    BipedAnimAttachSkinnedObject_t OriginalBipedAnimAttachSkinnedObject = nullptr;

    using BipedAnimAttachBipedWeapon_t = void (*)(RE::BipedAnim* biped, const RE::BGSObjectInstanceT<RE::TESObjectWEAP>& weapon, RE::BGSEquipIndex equipIndex);
    BipedAnimAttachBipedWeapon_t OriginalBipedAnimAttachBipedWeapon = nullptr;

    using BipedAnimAttachToParent_t = void (*)(RE::NiAVObject* parent, RE::NiAVObject* attachedObject, RE::NiAVObject* sourceObject, RE::BSTSmartPointer<RE::BipedAnim>& biped, RE::BIPED_OBJECT bipedObject);
    BipedAnimAttachToParent_t OriginalBipedAnimAttachToParent = nullptr;

    using BipedAnimRemovePart_t = void (*)(RE::BipedAnim* biped, RE::BIPOBJECT* bipObject, bool queueDetach);
    BipedAnimRemovePart_t OriginalBipedAnimRemovePart = nullptr;

    using ActorLoad3D_t = RE::NiAVObject* (*)(RE::TESObjectREFR* ref, bool backgroundLoading);
    ActorLoad3D_t OriginalActorLoad3D = nullptr;
    ActorLoad3D_t OriginalPlayerCharacterLoad3D = nullptr;

    // TESObjectREFR vfuncs we hook to widen actor-tag coverage past Load3D:
    //   0x88 Set3D                ? every "this ref now has a different 3D root"
    //   0x98 OnHeadInitialized    ? async FaceGen completion (head-only path)
    using Set3D_t = void (*)(RE::TESObjectREFR* ref, RE::NiAVObject* a_object, bool a_queue3DTasks);
    Set3D_t OriginalActorSet3D = nullptr;
    Set3D_t OriginalPlayerCharacterSet3D = nullptr;

    using OnHeadInitialized_t = void (*)(RE::TESObjectREFR* ref);
    OnHeadInitialized_t OriginalActorOnHeadInitialized = nullptr;
    OnHeadInitialized_t OriginalPlayerCharacterOnHeadInitialized = nullptr;

    // AIProcess::Update3dModel ? the engine's runtime model-rebuild entry
    // (race/sex change, NPC template re-roll, Reset3D/queue completion, PA
    // enter/exit). IDs from tools/og_to_ae_function_mapping.csv.
    using Update3DModel_t = void (*)(void* middleProcess, RE::Actor* actor, bool flag);
    Update3DModel_t OriginalUpdate3DModel = nullptr;

    // Actor::Reset3D ? script/console-driven full reload (disable;enable,
    // ResetActorReference). Eventually queues a Load3D on the loader thread.
    using Reset3D_t = void (*)(RE::Actor* actor, bool a_reloadAll, std::uint32_t a_additionalFlags, bool a_queueReset, std::uint32_t a_excludeFlags);
    Reset3D_t OriginalReset3D = nullptr;

    // BSBatchRenderer::RenderCommandBufferPassesImpl - drives the command-buffer
    // replay path. We hook entry to scan the pass list and decide between two
    // strategies (see HookedRenderCommandBufferPassesImpl for the full story).
    // Mangled: ?RenderCommandBufferPassesImpl@BSBatchRenderer@@QEAAXHPEAUCommandBufferPassesData@1@I_N@Z
    using RenderBatches_t = void (*)(void* this_, unsigned int passGroupIdx, bool allowAlpha, unsigned int filter);
    RenderBatches_t OriginalRenderBatches = nullptr;
    RenderBatches_t OriginalFinishShadowRenderBatches = nullptr;

    using RenderGeometryGroup_t = void (*)(void* accumulator, unsigned int group, bool allowAlpha);
    RenderGeometryGroup_t OriginalRenderGeometryGroup = nullptr;
    RenderGeometryGroup_t OriginalFinishShadowRenderGeometryGroup = nullptr;
    constexpr bool kInstallFinishShadowRenderSplitHooks = false;

    using RenderCommandBufferPassesImpl_t = void (*)(void* this_, int passGroupIdx, void* cbData, unsigned int subIdx, bool allowAlpha);
    RenderCommandBufferPassesImpl_t OriginalRenderCommandBufferPassesImpl = nullptr;

    using RenderPersistentPassListImpl_t = void (*)(void* persistentPassList, bool allowAlpha);
    RenderPersistentPassListImpl_t OriginalRenderPersistentPassListImpl = nullptr;

    using ProcessCommandBuffer_t = void (*)(void* renderer, void* cbData);
    ProcessCommandBuffer_t OriginalProcessCommandBuffer = nullptr;

    // BSBatchRenderer::RenderPassImpl - the immediate-path equivalent of
    // RenderCommandBufferPassesImpl. Calls RenderPassImmediately on the head
    // pass (which goes through BeginPass / state cleanup) and iterates the
    // passGroupNext chain calling RenderPassImmediatelySameTechnique for each.
    // Mangled: ?RenderPassImpl@BSBatchRenderer@@QEAAXPEAVBSRenderPass@@I_N@Z
    using RenderPassImpl_t = void (*)(void* this_, BSRenderPassLayout* head, std::uint32_t techniqueID, bool allowAlpha);
    RenderPassImpl_t OriginalRenderPassImpl = nullptr;

    using RegisterPassGeometryGroup_t = void (*)(void* batchRenderer, BSRenderPassLayout* pass, int group, bool appendOrUnk);
    RegisterPassGeometryGroup_t OriginalRegisterPassGeometryGroup = nullptr;

    // BSShader::BuildCommandBuffer - the chokepoint where command buffers are
    // built per pass. We hook this to inject one extra
    // CommandBufferShaderResource record (slot=DRAWTAG_SLOT, kind=0, stage=PS)
    // into the SRV table the engine memcpy's into the buffer. Because every
    // recorded entry in ProcessCommandBuffer's replay loop iterates the SRV
    // table and binds DRAWTAG_SLOT, this gives correct per-draw tag binding for
    // command-buffered draws ? without needing to intercept the replay itself.
    //
    // Implementation strategy: mutate BuildCommandBufferParam in-place before
    // calling original, then restore. The engine's MemoryManager::Allocate
    // computes size from srvCount so bumping it allocates room for our extra
    // record, and the existing memcpy(SRVs, srvSrc, 16*srvCount) inside
    // BuildCommandBuffer copies our extended array. No reallocation, no
    // platform-tail shifting, no lifetime tracking.
    //
    // Mangled: ?BuildCommandBuffer@BSShader@@QEAAPEAXAEAUBuildCommandBufferParam@1@@Z
    // Ghidra shows OG BuildCommandBuffer consumes the incoming R8 value when
    // touching the engine memory-manager init state. Preserve it across the
    // hook even though the public mangled signature only exposes one explicit
    // BuildCommandBufferParam reference.
    using BuildCommandBuffer_t = char* (*)(void* this_, void* param, void* memoryContext);
    BuildCommandBuffer_t OriginalBuildCommandBuffer = nullptr;

    // BSLight::TestFrustumCull ? engine's per-light frustum cull. We wrap it
    // so LightCullPolicy can temporarily scale geometry[+0x138] (the source
    // bound radius the cull function reads) for the duration of the engine's
    // call, then restore. See src/LightCullPolicy.{h,cpp} for the why.
    // Mangled: ?TestFrustumCull@BSLight@@QEAAIAEAVNiCullingProcess@@@Z
    // AE/NG IDs not yet mapped ? REL::ID's second slot is 0 and will fail to
    // resolve on AE; install is still attempted so the failure is loud rather
    // than silent. Fill in the AE ID in tools/og_ae_function_map.csv to enable.
    using BSLightTestFrustumCull_t = std::uint32_t (*)(void* light, void* cullingProcess);
    BSLightTestFrustumCull_t OriginalBSLightTestFrustumCull = nullptr;

    // BSDFTiledLighting::AddLight call-site redirect ? MULTIPLIES the radius
    // arg by the active scale so the tiled-buffer slot (offset +0x10 of each
    // 48-byte slot) sees the scaled radius. The downstream pDFTLCS compute
    // shader uses this for tile assignment + per-tile attenuation.
    //
    // Context: LightCullPolicy applies *transient* scaling around individual
    // engine consumers (cull-time and mesh-setup-time), restoring vanilla
    // immediately after each so shadow projection / fade-distance keep their
    // original values. By the time the lambda reads geometry[+0x138] for the
    // AddLight radius arg, the cull-side scale has already been restored ?
    // so the arg is vanilla. We multiply here to give the tiled-buffer slot
    // the same scaled value the per-volume mesh sees.
    //
    // The function has only ONE caller (this lambda); patch the `call rel32`
    // at lambda+0x281 directly so AddLight's RIP-relative prologue stays
    // untouched.
    using BSDFTiledLightingAddLight_t = void (*)(std::uint32_t id,
                                                 const RE::NiPoint3* pos,
                                                 float radius,
                                                 const RE::NiColor* color,
                                                 const RE::NiPoint3* dir,
                                                 bool flagA, bool flagB, bool flagC, bool flagD);
    BSDFTiledLightingAddLight_t OriginalBSDFTiledLightingAddLight = nullptr;

    // BSDFLightShader::SetupPointLightGeometry ? per-volume rasterization
    // setup. Called from DrawWorld::DrawPointLight to bind the technique,
    // build the world matrix from the light's bounding sphere, and push
    // per-light constants. Internally calls SetupPointLightTransforms which
    // reads geometry[+0x138] to size the rasterized sphere mesh. Hooking the
    // outer function lets us scale the source bound radius for the duration
    // of the entire mesh-setup chain, then restore ? so the volume mesh
    // expands but shadow projection (which runs outside this window) sees
    // the vanilla radius.
    // Mangled: ?SetupPointLightGeometry@BSDFLightShader@@QEAA_NPEAVBSLight@@I_N@Z
    using SetupPointLightGeometry_t = bool (*)(void* shader, void* light, std::uint32_t mask, char isUnk);
    SetupPointLightGeometry_t OriginalSetupPointLightGeometry = nullptr;

    thread_local float g_currentDrawTag = static_cast<float>(DrawMaterialTag::kUnknown);
    thread_local float g_currentDrawTagIsHead = 0.0f;
    thread_local std::uint32_t g_currentDrawTagRaceGroupMask = 0;
    thread_local std::uint32_t g_currentDrawTagRaceFlags = 0;
    thread_local std::vector<float> g_drawTagStack;
    thread_local std::vector<float> g_drawTagIsHeadStack;
    thread_local std::vector<std::uint32_t> g_drawTagRaceGroupMaskStack;
    thread_local std::vector<std::uint32_t> g_drawTagRaceFlagsStack;
    std::unordered_set<RE::BSGeometry*> g_actorDrawTaggedGeometry;
    // Subset of g_actorDrawTaggedGeometry consisting of non-hair geometry
    // reachable from the actor's BSFaceGenNiNode subtree. Used by the
    // pixel-shader-side isHead flag so toon face shading and facegen-aware eye
    // logic can run without re-tagging materialTag.
    std::unordered_set<RE::BSGeometry*> g_actorHeadDrawTaggedGeometry;
    std::unordered_map<std::uint32_t, std::unordered_set<RE::BSGeometry*>> g_actorDrawTaggedGeometryByRef;
    // All BSGeometry reachable from each ref's BSFaceGenNiNode subtree,
    // including hair. Used only for head-only refresh diffs so swapped hair can
    // still be added/removed as actor geometry without setting isHead.
    std::unordered_map<std::uint32_t, std::unordered_set<RE::BSGeometry*>> g_actorFaceDrawTaggedGeometryByRef;
    // Subset of g_actorDrawTaggedGeometryByRef limited to BSGeometry reachable
    // from each ref's BSFaceGenNiNode subtree after filtering hair headparts.
    // Maintained alongside the full per-ref set during full refreshes; mutated
    // alone during head-only refreshes.
    std::unordered_map<std::uint32_t, std::unordered_set<RE::BSGeometry*>> g_actorHeadDrawTaggedGeometryByRef;
    std::unordered_map<RE::BSGeometry*, std::uint32_t> g_actorRaceGroupMaskByGeometry;
    std::unordered_map<RE::BSGeometry*, std::uint32_t> g_actorRaceFlagsByGeometry;
    std::shared_mutex g_actorDrawTaggedGeometryLock;
    thread_local unsigned int g_renderBatchesGroup = UINT_MAX;
    thread_local std::uint32_t g_renderBatchesDepth = 0;

    std::uint32_t MakeDeferredCommandBufferDetailKey(int passGroupIdx, unsigned int subIdx)
    {
        const auto group = passGroupIdx >= 0 ? static_cast<std::uint32_t>(passGroupIdx) : g_renderBatchesGroup;
        return (group << 5) | std::min<std::uint32_t>(subIdx, 31u);
    }

    class ScopedDeferredPrePassDetail
    {
    public:
        ScopedDeferredPrePassDetail(PhaseTelemetry::DeferredPrePassDetailKind kind, std::uint32_t key) :
            kind_(kind),
            key_(key)
        {
            PhaseTelemetry::BeginDeferredPrePassDetail(kind_, key_);
        }

        ~ScopedDeferredPrePassDetail()
        {
            PhaseTelemetry::EndDeferredPrePassDetail(kind_, key_);
        }

        ScopedDeferredPrePassDetail(const ScopedDeferredPrePassDetail&) = delete;
        ScopedDeferredPrePassDetail& operator=(const ScopedDeferredPrePassDetail&) = delete;

    private:
        PhaseTelemetry::DeferredPrePassDetailKind kind_;
        std::uint32_t key_;
    };

    void SetCommandBufferDrawTagWrapper(RE::BSGeometry* geometry, const DrawTagClassification& classification);
    void ResetCommandBufferDrawTagWrappers();
    FakeStructuredResource* GetCommandBufferDrawTagWrapper(RE::BSGeometry* geometry, const DrawTagClassification& classification);
    bool EnsureDrawTagWrapperResources();

    void CollectActorDrawTaggedGeometry(RE::NiAVObject* root, std::unordered_set<RE::BSGeometry*>& geometry)
    {
        if (!root) {
            return;
        }

        if (netimmerse_cast<RE::NiBillboardNode*>(root)) {
            if (DEBUGGING) {
                REX::INFO("ActorDrawTagScan: skip billboard node='{}'", root->name.c_str());
            }
            return;
        }

        if (auto* geom = root->IsGeometry()) {
            const bool inserted = geometry.insert(geom).second;
            if (DEBUGGING) {
                REX::INFO(
                    "ActorDrawTagScan: {} geometry={} name='{}'",
                    inserted ? "insert" : "duplicate",
                    static_cast<void*>(geom),
                    geom->name.c_str());
            }
            return;
        }

        if (auto* node = root->IsNode()) {
            if (DEBUGGING) {
                REX::INFO("ActorDrawTagScan: descend node='{}' children={}", root->name.c_str(), node->children.size());
            }
            for (auto& child : node->children) {
                CollectActorDrawTaggedGeometry(child.get(), geometry);
            }
            return;
        }

        if (DEBUGGING) {
            REX::INFO("ActorDrawTagScan: skip unknown name='{}' (neither node nor geometry)", root->name.c_str());
        }
    }

    std::string MakeLowerAscii(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (unsigned char ch : text) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
        return out;
    }

    void AddHeadPartMatchKey(std::string_view text, std::unordered_set<std::string>& keys)
    {
        auto key = MakeLowerAscii(text);
        if (key.empty()) {
            return;
        }

        std::ranges::replace(key, '/', '\\');
        if (const auto slash = key.find_last_of('\\'); slash != std::string::npos) {
            key.erase(0, slash + 1);
        }
        if (const auto dot = key.find_last_of('.'); dot != std::string::npos) {
            key.erase(dot);
        }
        if (key.size() >= 4) {
            keys.insert(std::move(key));
        }
    }

    void AddHairHeadPartMatchKeys(RE::BGSHeadPart* headPart, std::unordered_set<std::string>& keys)
    {
        if (!headPart) {
            return;
        }

        AddHeadPartMatchKey(headPart->ChargenModel.GetModel(), keys);
        AddHeadPartMatchKey(headPart->formEditorID.c_str(), keys);

        for (auto* extraPart : headPart->extraParts) {
            AddHairHeadPartMatchKeys(extraPart, keys);
        }
    }

    std::unordered_set<std::string> BuildHairHeadPartMatchKeys(RE::TESObjectREFR* ref)
    {
        std::unordered_set<std::string> keys;
        auto* base = ref ? ref->GetObjectReference() : nullptr;
        auto* npc = base ? base->As<RE::TESNPC>() : nullptr;
        if (!npc || npc->numHeadParts <= 0 || !npc->headParts) {
            return keys;
        }

        // Race changes can leave the player alternate-head-part state transient
        // while Reset3D/head tagging runs. Avoid TESNPC::GetHeadParts(true)
        // here because it consults that global alternate map and has crashed
        // in UsingAlternateHeadPartList during race-change resets.
        const std::span headParts{ npc->headParts, static_cast<std::size_t>(npc->numHeadParts) };
        for (auto* headPart : headParts) {
            if (!headPart ||
                headPart->type.get() != RE::BGSHeadPart::HeadPartType::kHair) {
                continue;
            }
            AddHairHeadPartMatchKeys(headPart, keys);
        }

        return keys;
    }

    bool MatchesHairHeadPartNode(RE::NiAVObject* object, const std::unordered_set<std::string>& hairHeadPartKeys)
    {
        if (!object || hairHeadPartKeys.empty()) {
            return false;
        }

        const auto name = MakeLowerAscii(object->name.c_str());
        if (name.empty()) {
            return false;
        }

        for (const auto& key : hairHeadPartKeys) {
            if (name.find(key) != std::string::npos) {
                return true;
            }
        }

        return false;
    }

    void CollectActorHeadDrawTaggedGeometry(
        RE::NiAVObject* root,
        const std::unordered_set<std::string>& hairHeadPartKeys,
        std::unordered_set<RE::BSGeometry*>& geometry)
    {
        if (!root) {
            return;
        }

        if (MatchesHairHeadPartNode(root, hairHeadPartKeys)) {
            if (DEBUGGING) {
                REX::INFO("ActorHeadDrawTagScan: skip hair headpart subtree node='{}'", root->name.c_str());
            }
            return;
        }

        if (netimmerse_cast<RE::NiBillboardNode*>(root)) {
            if (DEBUGGING) {
                REX::INFO("ActorHeadDrawTagScan: skip billboard node='{}'", root->name.c_str());
            }
            return;
        }

        if (auto* geom = root->IsGeometry()) {
            geometry.insert(geom);
            return;
        }

        if (auto* node = root->IsNode()) {
            for (auto& child : node->children) {
                CollectActorHeadDrawTaggedGeometry(child.get(), hairHeadPartKeys, geometry);
            }
        }
    }

    void ResolveRaceGroupsIfNeeded()
    {
        {
            std::shared_lock lock(g_raceGroupLock);
            if (g_raceGroupsResolved) {
                return;
            }
        }

        std::unique_lock lock(g_raceGroupLock);
        if (g_raceGroupsResolved) {
            return;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            return;
        }

        g_raceGroupMaskByRaceFormID.clear();
        for (const auto& ref : g_raceGroupFormRefs) {
            if (ref.pluginName.empty() || ref.formID == 0 || ref.groupMask == 0) {
                continue;
            }

            auto* race = dataHandler->LookupForm<RE::TESRace>(ref.formID, ref.pluginName);
            if (!race) {
                REX::WARN(
                    "ResolveRaceGroups: could not resolve race {}|0x{:X}",
                    ref.pluginName,
                    ref.formID);
                continue;
            }

            g_raceGroupMaskByRaceFormID[race->GetFormID()] |= ref.groupMask;
        }

        g_raceGroupsResolved = true;
        REX::INFO(
            "ResolveRaceGroups: resolved {} configured race reference(s) into {} race form(s)",
            g_raceGroupFormRefs.size(),
            g_raceGroupMaskByRaceFormID.size());
    }

    std::pair<std::uint32_t, std::uint32_t> GetRaceTagForRace(RE::TESRace* race)
    {
        if (!race) {
            return { 0, 0 };
        }

        ResolveRaceGroupsIfNeeded();

        std::uint32_t mask = 0;
        {
            std::shared_lock lock(g_raceGroupLock);
            auto it = g_raceGroupMaskByRaceFormID.find(race->GetFormID());
            if (it != g_raceGroupMaskByRaceFormID.end()) {
                mask = it->second;
            }
        }

        return { mask, kDrawTagRaceResolved };
    }

    DrawTagClassification MakeUnknownDrawTagClassification()
    {
        return {};
    }

    DrawTagClassification MakeActorDrawTagClassification(bool isHead, std::uint32_t raceGroupMask, std::uint32_t raceFlags)
    {
        DrawTagClassification out{};
        out.materialTag = static_cast<float>(DrawMaterialTag::kActor);
        out.isHead = isHead ? 1.0f : 0.0f;
        out.raceGroupMask = raceGroupMask;
        out.raceFlags = raceFlags;
        return out;
    }

    void RemoveActorDrawTaggedGeometry(RE::NiAVObject* root)
    {
        if (!root) {
            return;
        }

        std::unique_lock lock(g_actorDrawTaggedGeometryLock);
        RE::BSVisit::TraverseScenegraphGeometries(root, [](RE::BSGeometry* geometry) {
            if (geometry) {
                g_actorDrawTaggedGeometry.erase(geometry);
                g_actorHeadDrawTaggedGeometry.erase(geometry);
                g_actorRaceGroupMaskByGeometry.erase(geometry);
                g_actorRaceFlagsByGeometry.erase(geometry);
                SetCommandBufferDrawTagWrapper(geometry, MakeUnknownDrawTagClassification());
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
    }

    void RefreshActorDrawTaggedGeometry(RE::TESObjectREFR* ref, RE::TESRace* race)
    {
        if (!ref) {
            return;
        }

        const auto handle = ref->GetHandle().get_handle();
        if (!handle) {
            return;
        }

        std::unordered_set<RE::BSGeometry*> liveGeometry;
        CollectActorDrawTaggedGeometry(ref->Get3D(), liveGeometry);

        if (ref == RE::PlayerCharacter::GetSingleton()) {
            bool isFirstPerson = ref->Get3D(true) == ref->Get3D();
            CollectActorDrawTaggedGeometry(ref->Get3D(!isFirstPerson), liveGeometry);
        }

        // Snapshot the face subtree separately so a later head-only refresh
        // has a baseline to diff against. The head subset filters hair out.
        std::unordered_set<RE::BSGeometry*> liveFaceGeometry;
        std::unordered_set<RE::BSGeometry*> liveHeadGeometry;
        if (auto* faceRaw = ref->GetFaceNodeSkinned()) {
            CollectActorDrawTaggedGeometry(reinterpret_cast<RE::NiAVObject*>(faceRaw), liveFaceGeometry);
            CollectActorHeadDrawTaggedGeometry(
                reinterpret_cast<RE::NiAVObject*>(faceRaw),
                BuildHairHeadPartMatchKeys(ref),
                liveHeadGeometry);
        }

        const auto [raceGroupMask, raceFlags] = GetRaceTagForRace(race);

        std::unique_lock lock(g_actorDrawTaggedGeometryLock);
        auto& currentSet = g_actorDrawTaggedGeometryByRef[handle];

        auto& currentFaceSet = g_actorFaceDrawTaggedGeometryByRef[handle];

        auto& currentHeadSet = g_actorHeadDrawTaggedGeometryByRef[handle];

        // Diff: drop tags whose BSGeometry is no longer reachable in the
        // actor's scenegraph. This avoids the brief untagged window that a
        // wholesale clear-then-readd would produce on body geometry whenever
        // a non-body event (head rebuild, weapon attach, etc.) fires.
        for (auto it = currentSet.begin(); it != currentSet.end(); ) {
            if (!liveGeometry.contains(*it)) {
                g_actorDrawTaggedGeometry.erase(*it);
                currentFaceSet.erase(*it);
                g_actorHeadDrawTaggedGeometry.erase(*it);
                g_actorRaceGroupMaskByGeometry.erase(*it);
                g_actorRaceFlagsByGeometry.erase(*it);
                SetCommandBufferDrawTagWrapper(*it, MakeUnknownDrawTagClassification());
                it = currentSet.erase(it);
            } else {
                ++it;
            }
        }

        // Diff: tag geometry the walk just discovered. Use isHead only for
        // non-hair geometry reachable from BSFaceGenNiNode.
        for (auto* geometry : liveGeometry) {
            const bool isHead = liveHeadGeometry.contains(geometry);
            g_actorRaceGroupMaskByGeometry[geometry] = raceGroupMask;
            g_actorRaceFlagsByGeometry[geometry] = raceFlags;
            if (currentSet.insert(geometry).second) {
                g_actorDrawTaggedGeometry.insert(geometry);
                if (isHead) {
                    g_actorHeadDrawTaggedGeometry.insert(geometry);
                }
            } else {
                // Pre-existing entry ? head membership may have changed since
                // the previous full refresh (e.g. a weapon attach that runs
                // before head async completes). Resync the wrapper in case
                // the head subtree just shifted.
                const bool wasHead = currentHeadSet.contains(geometry);
                if (isHead != wasHead) {
                    if (isHead) {
                        g_actorHeadDrawTaggedGeometry.insert(geometry);
                    } else {
                        g_actorHeadDrawTaggedGeometry.erase(geometry);
                    }
                }
            }
            SetCommandBufferDrawTagWrapper(
                geometry,
                MakeActorDrawTagClassification(isHead, raceGroupMask, raceFlags));
        }

        currentFaceSet = std::move(liveFaceGeometry);
        currentHeadSet = std::move(liveHeadGeometry);
    }

    // Cheaper variant for FaceGen/head-only events: walks just the face node
    // and diffs against the per-ref head subset. Body/equipment tags are never
    // touched, so high-frequency head events (FaceGen async completion,
    // expression rebuilds) cannot flicker body draws.
    void RefreshActorHeadDrawTaggedGeometry(RE::TESObjectREFR* ref, RE::TESRace* race)
    {
        if (!ref) {
            return;
        }

        auto* faceRaw = ref->GetFaceNodeSkinned();
        if (!faceRaw) {
            return;
        }

        const auto handle = ref->GetHandle().get_handle();
        if (!handle) {
            return;
        }

        std::unordered_set<RE::BSGeometry*> liveFaceGeometry;
        CollectActorDrawTaggedGeometry(reinterpret_cast<RE::NiAVObject*>(faceRaw), liveFaceGeometry);

        std::unordered_set<RE::BSGeometry*> liveHeadGeometry;
        CollectActorHeadDrawTaggedGeometry(
            reinterpret_cast<RE::NiAVObject*>(faceRaw),
            BuildHairHeadPartMatchKeys(ref),
            liveHeadGeometry);

        const auto [raceGroupMask, raceFlags] = GetRaceTagForRace(race);

        std::unique_lock lock(g_actorDrawTaggedGeometryLock);
        auto& currentFullSet = g_actorDrawTaggedGeometryByRef[handle];
        auto& currentFaceSet = g_actorFaceDrawTaggedGeometryByRef[handle];
        auto& currentHeadSet = g_actorHeadDrawTaggedGeometryByRef[handle];

        // Diff: remove old FaceGen entries that are no longer present under the
        // face node. Hair is included here for actor-tag cleanup, but it is not
        // included in currentHeadSet or g_actorHeadDrawTaggedGeometry.
        for (auto it = currentFaceSet.begin(); it != currentFaceSet.end(); ) {
            if (!liveFaceGeometry.contains(*it)) {
                g_actorDrawTaggedGeometry.erase(*it);
                g_actorHeadDrawTaggedGeometry.erase(*it);
                g_actorRaceGroupMaskByGeometry.erase(*it);
                g_actorRaceFlagsByGeometry.erase(*it);
                SetCommandBufferDrawTagWrapper(*it, MakeUnknownDrawTagClassification());
                currentFullSet.erase(*it);
                currentHeadSet.erase(*it);
                it = currentFaceSet.erase(it);
            } else {
                ++it;
            }
        }

        // Diff: add/update live FaceGen entries. Hair remains actor-tagged but
        // is not added to the head subset, so shaders see isHead = 0.
        for (auto* geometry : liveFaceGeometry) {
            const bool isHead = liveHeadGeometry.contains(geometry);
            currentFaceSet.insert(geometry);
            currentFullSet.insert(geometry);
            g_actorDrawTaggedGeometry.insert(geometry);
            if (isHead) {
                currentHeadSet.insert(geometry);
                g_actorHeadDrawTaggedGeometry.insert(geometry);
            } else {
                currentHeadSet.erase(geometry);
                g_actorHeadDrawTaggedGeometry.erase(geometry);
            }
            g_actorRaceGroupMaskByGeometry[geometry] = raceGroupMask;
            g_actorRaceFlagsByGeometry[geometry] = raceFlags;
            SetCommandBufferDrawTagWrapper(
                geometry,
                MakeActorDrawTagClassification(isHead, raceGroupMask, raceFlags));
        }
    }

    void RefreshActorDrawTaggedGeometry(RE::Actor* actor)
    {
        RefreshActorDrawTaggedGeometry(
            static_cast<RE::TESObjectREFR*>(actor),
            actor ? actor->race : nullptr);
    }

    void RefreshActorHeadDrawTaggedGeometry(RE::Actor* actor)
    {
        RefreshActorHeadDrawTaggedGeometry(
            static_cast<RE::TESObjectREFR*>(actor),
            actor ? actor->race : nullptr);
    }

    void RefreshActorDrawTaggedGeometry(RE::BipedAnim* biped)
    {
        if (!biped) {
            return;
        }

        auto ref = biped->GetRequester().get();
        auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
        RefreshActorDrawTaggedGeometry(actor);
    }

    DrawTagClassification ClassifyDrawTag(BSRenderPassLayout* pass)
    {
        DrawTagClassification out{};
        out.materialTag = static_cast<float>(DrawMaterialTag::kUnknown);
        out.isHead = 0.0f;

        if (!pass || !pass->geometry) {
            return out;
        }

        std::shared_lock lock(g_actorDrawTaggedGeometryLock);
        if (g_actorDrawTaggedGeometry.contains(pass->geometry)) {
            out.materialTag = static_cast<float>(DrawMaterialTag::kActor);
            if (g_actorHeadDrawTaggedGeometry.contains(pass->geometry)) {
                out.isHead = 1.0f;
            }
            if (auto it = g_actorRaceGroupMaskByGeometry.find(pass->geometry);
                it != g_actorRaceGroupMaskByGeometry.end()) {
                out.raceGroupMask = it->second;
            }
            if (auto it = g_actorRaceFlagsByGeometry.find(pass->geometry);
                it != g_actorRaceFlagsByGeometry.end()) {
                out.raceFlags = it->second;
            }
        }

        return out;
    }

    StaticDrawTagKey MakeStaticDrawTagKey(const DrawTagClassification& classification)
    {
        return StaticDrawTagKey{
            static_cast<std::uint32_t>(classification.materialTag + 0.5f),
            static_cast<std::uint32_t>(classification.isHead + 0.5f),
            classification.raceGroupMask,
            classification.raceFlags
        };
    }

    REX::W32::ID3D11ShaderResourceView* GetOrCreateStaticDrawTagSRV(const DrawTagClassification& classification);

    void SetCommandBufferDrawTagWrapper(RE::BSGeometry* geometry, const DrawTagClassification& classification)
    {
        if (!geometry || !g_tagWrapperResourcesReady) {
            return;
        }

        std::unique_lock lock(g_commandBufferDrawTagWrappersLock);
        auto it = g_commandBufferDrawTagWrappers.find(geometry);
        if (it == g_commandBufferDrawTagWrappers.end()) {
            if (classification.materialTag < 0.5f &&
                classification.raceGroupMask == 0 &&
                classification.raceFlags == 0) {
                return;
            }
            it = g_commandBufferDrawTagWrappers.emplace(geometry, nullptr).first;
        }

        auto& wrapper = it->second;
        if (!wrapper) {
            wrapper = std::make_unique<FakeStructuredResource>();
            wrapper->fenceCount = 0;
        }

        wrapper->srv.store(GetOrCreateStaticDrawTagSRV(classification), std::memory_order_release);
    }

    void ResetCommandBufferDrawTagWrappers()
    {
        std::unique_lock lock(g_commandBufferDrawTagWrappersLock);
        for (auto& [geometry, wrapper] : g_commandBufferDrawTagWrappers) {
            if (wrapper) {
                wrapper->srv.store(g_unknownTagSRV, std::memory_order_release);
            }
        }
    }

    FakeStructuredResource* GetCommandBufferDrawTagWrapper(RE::BSGeometry* geometry, const DrawTagClassification& classification)
    {
        if (!geometry) {
            return &g_unknownTagWrapper;
        }

        std::unique_lock lock(g_commandBufferDrawTagWrappersLock);
        auto& wrapper = g_commandBufferDrawTagWrappers[geometry];
        if (!wrapper) {
            wrapper = std::make_unique<FakeStructuredResource>();
            wrapper->fenceCount = 0;
        }

        wrapper->srv.store(GetOrCreateStaticDrawTagSRV(classification), std::memory_order_release);
        return wrapper.get();
    }

    void PushCurrentDrawTag(BSRenderPassLayout* pass)
    {
        g_drawTagStack.push_back(g_currentDrawTag);
        g_drawTagIsHeadStack.push_back(g_currentDrawTagIsHead);
        g_drawTagRaceGroupMaskStack.push_back(g_currentDrawTagRaceGroupMask);
        g_drawTagRaceFlagsStack.push_back(g_currentDrawTagRaceFlags);
        const auto classification = ClassifyDrawTag(pass);
        g_currentDrawTag = classification.materialTag;
        g_currentDrawTagIsHead = classification.isHead;
        g_currentDrawTagRaceGroupMask = classification.raceGroupMask;
        g_currentDrawTagRaceFlags = classification.raceFlags;
    }

    void PopCurrentDrawTag()
    {
        if (!g_drawTagStack.empty()) {
            g_currentDrawTag = g_drawTagStack.back();
            g_drawTagStack.pop_back();
        } else {
            g_currentDrawTag = static_cast<float>(DrawMaterialTag::kUnknown);
        }

        if (!g_drawTagIsHeadStack.empty()) {
            g_currentDrawTagIsHead = g_drawTagIsHeadStack.back();
            g_drawTagIsHeadStack.pop_back();
        } else {
            g_currentDrawTagIsHead = 0.0f;
        }

        if (!g_drawTagRaceGroupMaskStack.empty()) {
            g_currentDrawTagRaceGroupMask = g_drawTagRaceGroupMaskStack.back();
            g_drawTagRaceGroupMaskStack.pop_back();
        } else {
            g_currentDrawTagRaceGroupMask = 0;
        }

        if (!g_drawTagRaceFlagsStack.empty()) {
            g_currentDrawTagRaceFlags = g_drawTagRaceFlagsStack.back();
            g_drawTagRaceFlagsStack.pop_back();
        } else {
            g_currentDrawTagRaceFlags = 0;
        }
    }

    void HookedBSLightingShaderSetupGeometry(RE::BSShader* shader, BSRenderPassLayout* pass)
    {
        PushCurrentDrawTag(pass);
        OriginalBSLightingShaderSetupGeometry(shader, pass);
    }

    void HookedBSLightingShaderRestoreGeometry(RE::BSShader* shader, BSRenderPassLayout* pass)
    {
        OriginalBSLightingShaderRestoreGeometry(shader, pass);
        PopCurrentDrawTag();
    }

    void HookedBSEffectShaderSetupGeometry(RE::BSShader* shader, BSRenderPassLayout* pass)
    {
        PushCurrentDrawTag(pass);
        OriginalBSEffectShaderSetupGeometry(shader, pass);
    }

    void HookedBSEffectShaderRestoreGeometry(RE::BSShader* shader, BSRenderPassLayout* pass)
    {
        OriginalBSEffectShaderRestoreGeometry(shader, pass);
        PopCurrentDrawTag();
    }

    void HookedBSBatchRendererDraw(BSRenderPassLayout* pass, std::uintptr_t unk2, std::uintptr_t unk3, RE::BSGraphics::DynamicTriShapeDrawData* dynamicDrawData)
    {
        D3D11Hooks::EnsureDrawHooksPresent();

        // Per-phase G-buffer telemetry. Cheap when INI is `off`. Attributes
        // this draw to whichever DrawWorld:: sub-phase is innermost on the TLS
        // stack (no-op if outside Render_PreUI entirely).
        if (PhaseTelemetry::g_mode.load(std::memory_order_relaxed) == PhaseTelemetry::Mode::On) {
            PhaseTelemetry::OnDraw();
        }
        if (ShadowTelemetry::g_mode.load(std::memory_order_relaxed) == ShadowTelemetry::Mode::On) {
            ShadowTelemetry::OnBSDraw();
        }

        const bool needsDrawTag = ShaderResources::ActiveReplacementPixelShaderUsesDrawTag() || DEBUGGING;
        if (needsDrawTag) {
            PushCurrentDrawTag(pass);
        }

        if (DEBUGGING) {
            const bool actorTag = g_currentDrawTag >= 0.5f;
            if (actorTag && pass && pass->geometry) {
                struct DrawKey {
                    RE::BSGeometry* geometry;
                    REX::W32::ID3D11PixelShader* shader;
                    std::uint32_t techniqueID;
                    bool operator==(const DrawKey& o) const noexcept {
                        return geometry == o.geometry && shader == o.shader && techniqueID == o.techniqueID;
                    }
                };
                struct DrawKeyHash {
                    std::size_t operator()(const DrawKey& k) const noexcept {
                        std::size_t h = std::hash<void*>{}(k.geometry);
                        h ^= std::hash<void*>{}(k.shader) + 0x9e3779b9 + (h << 6) + (h >> 2);
                        h ^= std::hash<std::uint32_t>{}(k.techniqueID) + 0x9e3779b9 + (h << 6) + (h >> 2);
                        return h;
                    }
                };

                static std::unordered_set<DrawKey, DrawKeyHash> loggedPairs;
                static std::mutex loggedPairsMutex;

                auto* originalShader = g_currentOriginalPixelShader.load(std::memory_order_acquire);
                const DrawKey key{ pass->geometry, originalShader, pass->techniqueID };

                bool shouldLog = false;
                {
                    std::lock_guard lock(loggedPairsMutex);
                    shouldLog = loggedPairs.insert(key).second;
                }

                if (shouldLog) {
                    std::string shaderUID = "<no PS>";
                    std::string defId = "<unmatched>";
                    if (originalShader) {
                        std::shared_lock entryLock(g_ShaderDB.mutex);
                        auto it = g_ShaderDB.entries.find(originalShader);
                        if (it != g_ShaderDB.entries.end()) {
                            shaderUID = it->second.shaderUID.empty() ? "<no UID>" : it->second.shaderUID;
                            if (it->second.matchedDefinition) {
                                defId = it->second.matchedDefinition->id;
                            }
                        } else {
                            shaderUID = "<no DB entry>";
                        }
                    }

                    REX::INFO(
                        "ActorDrawTag: geometry={} name='{}' techniqueID=0x{:08X} psShader={} shaderUID={} matchedDef={}",
                        static_cast<void*>(pass->geometry),
                        pass->geometry->name.c_str(),
                        pass->techniqueID,
                        static_cast<void*>(originalShader),
                        shaderUID,
                        defId);
                }
            }
        }

        // Bind+update via the immediate context here, because in some configurations
        // (ENB / ReShade-wrapped contexts, command-buffer replay paths) the D3D11
        // vtable hooks on g_rendererData->context don't fire for actor draws even
        // though BSBatchRenderer::Draw does. Doing it engine-side guarantees the
        // tag buffer is current for every pass that classified as actor.
        if (g_rendererData && g_rendererData->context) {
            BindDrawTagForCurrentDraw(g_rendererData->context, true);
            FireArmedCustomPassDrawBatch(g_rendererData->context, "engine-BSBatch");
            if (SHADERENGINE_EFFECTS_ON && ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind()) {
                ShaderResources::BindInjectedPixelShaderResources(g_rendererData->context);
            }
        }

        if (!InteriorDeferredLighting::TryRender(
                pass,
                unk2,
                unk3,
                dynamicDrawData,
                OriginalBSBatchRendererDraw)) {
            OriginalBSBatchRendererDraw(pass, unk2, unk3, dynamicDrawData);
        }
        if (needsDrawTag) {
            PopCurrentDrawTag();
        }
    }

    RE::NiAVObject* HookedBipedAnimAttachSkinnedObject(RE::BipedAnim* biped, RE::NiNode* destinationRoot, RE::NiNode* sourceRoot, RE::BIPED_OBJECT bipedObject, bool firstPerson)
    {
        auto* attachedObject = OriginalBipedAnimAttachSkinnedObject(biped, destinationRoot, sourceRoot, bipedObject, firstPerson);
        RefreshActorDrawTaggedGeometry(biped);
        return attachedObject;
    }

    void HookedBipedAnimAttachBipedWeapon(RE::BipedAnim* biped, const RE::BGSObjectInstanceT<RE::TESObjectWEAP>& weapon, RE::BGSEquipIndex equipIndex)
    {
        OriginalBipedAnimAttachBipedWeapon(biped, weapon, equipIndex);
        RefreshActorDrawTaggedGeometry(biped);
    }

    void HookedBipedAnimAttachToParent(RE::NiAVObject* parent, RE::NiAVObject* attachedObject, RE::NiAVObject* sourceObject, RE::BSTSmartPointer<RE::BipedAnim>& biped, RE::BIPED_OBJECT bipedObject)
    {
        OriginalBipedAnimAttachToParent(parent, attachedObject, sourceObject, biped, bipedObject);

        if (biped && bipedObject != RE::BIPED_OBJECT::kTotal) {
            RefreshActorDrawTaggedGeometry(biped.get());
        }
    }

    void HookedBipedAnimRemovePart(RE::BipedAnim* biped, RE::BIPOBJECT* bipObject, bool queueDetach)
    {
        if (bipObject) {
            RemoveActorDrawTaggedGeometry(bipObject->partClone.get());
        }

        OriginalBipedAnimRemovePart(biped, bipObject, queueDetach);
        RefreshActorDrawTaggedGeometry(biped);
    }

    void HookedFinishShadowRenderBatches(void* this_, unsigned int passGroupIdx, bool allowAlpha, unsigned int filter)
    {
        auto renderBatches = OriginalRenderBatches ? OriginalRenderBatches : OriginalFinishShadowRenderBatches;
        if (!renderBatches) {
            return;
        }

        const unsigned int prevGroup = g_renderBatchesGroup;
        ++g_renderBatchesDepth;
        g_renderBatchesGroup = passGroupIdx;
        {
            if (!ShouldSplitRenderBatchesCall(passGroupIdx, filter)) {
                renderBatches(this_, passGroupIdx, allowAlpha, filter);
            } else {
                ScopedFinishLevelShadowSplit finishSplit;
                ScopedShadowBatchGroupExposure exposure(this_, passGroupIdx);
                if (!exposure.Empty()) {
                    renderBatches(this_, passGroupIdx, allowAlpha, filter);
                }
            }
        }
        g_renderBatchesGroup = prevGroup;
        --g_renderBatchesDepth;
    }

    void HookedFinishShadowRenderGeometryGroup(void* accumulator, unsigned int group, bool allowAlpha)
    {
        if (!OriginalFinishShadowRenderGeometryGroup) {
            return;
        }

        if (!IsShadowCacheRenderSplitActive() || group != 9 || !accumulator) {
            OriginalFinishShadowRenderGeometryGroup(accumulator, group, allowAlpha);
            return;
        }

        auto* geometryGroup = *reinterpret_cast<std::byte**>(
            static_cast<std::byte*>(accumulator) + 0x420 + static_cast<std::size_t>(group) * sizeof(void*));
        const bool persistentListMode =
            geometryGroup &&
            ((*reinterpret_cast<std::uint8_t*>(geometryGroup + kGeometryGroupFlagsOffset) & 1u) != 0);
        if (!persistentListMode) {
            OriginalFinishShadowRenderGeometryGroup(accumulator, group, allowAlpha);
            return;
        }

        {
            ScopedFinishLevelShadowSplit finishSplit;
            ScopedPersistentShadowListExposure exposure(geometryGroup + kGeometryGroupPersistentHeadOffset);
            OriginalFinishShadowRenderGeometryGroup(accumulator, group, allowAlpha);
        }
    }

    void HookedRenderBatches(void* this_, unsigned int passGroupIdx, bool allowAlpha, unsigned int filter)
    {
        ScopedDeferredPrePassDetail detail(
            PhaseTelemetry::DeferredPrePassDetailKind::RenderBatches,
            passGroupIdx);
        const unsigned int prevGroup = g_renderBatchesGroup;
        ++g_renderBatchesDepth;
        g_renderBatchesGroup = passGroupIdx;
        {
            if (ShouldSplitRenderBatchesCall(passGroupIdx, filter)) {
                ScopedFinishLevelShadowSplit renderBatchesSplit;
                ScopedShadowBatchGroupExposure exposure(this_, passGroupIdx);
                if (!exposure.Empty()) {
                    OriginalRenderBatches(this_, passGroupIdx, allowAlpha, filter);
                }
            } else {
                OriginalRenderBatches(this_, passGroupIdx, allowAlpha, filter);
            }
        }
        g_renderBatchesGroup = prevGroup;
        --g_renderBatchesDepth;
    }

    void HookedRenderGeometryGroup(void* accumulator, unsigned int group, bool allowAlpha)
    {
        ScopedDeferredPrePassDetail detail(
            PhaseTelemetry::DeferredPrePassDetailKind::RenderGeometryGroup,
            group);
        OriginalRenderGeometryGroup(accumulator, group, allowAlpha);
    }

    void HookedProcessCommandBuffer(void* renderer, void* cbData)
    {
        if (!tl_processCommandBufferTargetActive) {
            OriginalProcessCommandBuffer(renderer, cbData);
            return;
        }

        auto target = tl_processCommandBufferTarget;
        target.cbData = cbData;

        if (!IsShadowWorkTelemetryActive()) {
            OriginalProcessCommandBuffer(renderer, cbData);
            return;
        }

        ScopedShadowWork shadowWork(ShadowTelemetry::WorkKind::CommandBufferReplay, target);
        OriginalProcessCommandBuffer(renderer, cbData);
    }

    void HookedRenderCommandBufferPassesImpl(void* this_, int passGroupIdx, void* cbData, unsigned int subIdx, bool allowAlpha)
    {
        const auto detailKey = MakeDeferredCommandBufferDetailKey(passGroupIdx, subIdx);
        ScopedDeferredPrePassDetail detail(
            PhaseTelemetry::DeferredPrePassDetailKind::RenderCommandBufferPasses,
            detailKey);
        D3D11Hooks::EnsureDrawHooksPresent();

        if (PhaseTelemetry::g_mode.load(std::memory_order_relaxed) == PhaseTelemetry::Mode::On) {
            PhaseTelemetry::OnCommandBufferDraw();
        }
        if (ShadowTelemetry::g_mode.load(std::memory_order_relaxed) == ShadowTelemetry::Mode::On) {
            ShadowTelemetry::OnCommandBufferDraw();
        }

        auto* head = GetRenderBatchNodeHead(this_, passGroupIdx, subIdx);
        const auto node = GetRenderBatchNodeAddress(this_, passGroupIdx, subIdx);
        PhaseTelemetry::NoteDeferredPrePassCommandBuffer(
            detailKey,
            head,
            head ? static_cast<void*>(head->geometry) : nullptr,
            head ? static_cast<void*>(head->shader) : nullptr,
            node ? GetRenderBatchNodeTechniqueRaw(node) : (head ? head->techniqueID : 0),
            CountPassChain(head),
            (head && head->geometry) ? head->geometry->name.c_str() : "");

        {
            ActiveCommandBufferSelection fallbackSelection{};
            auto* shadowSelection =
                IsFinishLevelShadowSplitActive() ? FindActiveCommandBufferSelection(node) : nullptr;
            const bool forceSelectedReplay =
                IsFinishLevelShadowSplitActive() &&
                IsShadowCacheRenderSplitActive() &&
                (passGroupIdx == static_cast<int>(kShadowCacheBatchPassGroup) ||
                 ShadowTelemetry::IsShadowCacheActiveForCurrentShadow());
            if (forceSelectedReplay && !shadowSelection) {
                fallbackSelection.node = node;
                fallbackSelection.head = head;
                fallbackSelection.techniqueID = node ? GetRenderBatchNodeTechniqueRaw(node) : 0;
                fallbackSelection.commandBuffers = CollectCommandBufferRecords(head);
                shadowSelection = &fallbackSelection;
            }

            void* replayCbData = cbData;
            if (shadowSelection) {
                const auto phase = CurrentShadowSplitPhase();
                if (!shadowSelection->head) {
                    if (phase == ShadowSplitPhase::DynamicOverlay) {
                        InvalidateCommandBufferFrame(cbData);
                    }
                    return;
                }

                replayCbData = PrepareShadowCommandBufferSidecar(cbData, *shadowSelection, phase);
                if (!replayCbData) {
                    OriginalRenderPassImpl(
                        this_,
                        shadowSelection->head,
                        shadowSelection->techniqueID,
                        allowAlpha);
                    if (phase == ShadowSplitPhase::DynamicOverlay) {
                        InvalidateCommandBufferFrame(cbData);
                    }
                    return;
                }
            }

            ScopedCommandBufferPassesFilter shadowSplitFilter(this_, passGroupIdx, cbData, subIdx);
            if (shadowSplitFilter.Active()) {
                if (!shadowSplitFilter.Head()) {
                    InvalidateCommandBufferFrame(cbData);
                    return;
                }

                if (shadowSplitFilter.NeedsImmediateFallback()) {
                    OriginalRenderPassImpl(
                        this_,
                        shadowSplitFilter.Head(),
                        shadowSplitFilter.TechniqueID(),
                        allowAlpha);
                    InvalidateCommandBufferFrame(cbData);
                    return;
                }
            }

            ScopedDeferredPrePassStaticCommandBufferFilter deferredStaticFilter(
                this_,
                passGroupIdx,
                replayCbData,
                subIdx);
            if (deferredStaticFilter.Active()) {
                if (!deferredStaticFilter.Head()) {
                    InvalidateCommandBufferFrame(replayCbData);
                    return;
                }

                if (deferredStaticFilter.NeedsImmediateFallback()) {
                    OriginalRenderPassImpl(
                        this_,
                        deferredStaticFilter.Head(),
                        deferredStaticFilter.TechniqueID(),
                        allowAlpha);
                    InvalidateCommandBufferFrame(replayCbData);
                    return;
                }
            }

            std::optional<ScopedProcessCommandBufferTarget> processTarget;
            if (IsShadowWorkTelemetryActive()) {
                processTarget.emplace(MakeShadowPassWorkTarget(
                    this_,
                    shadowSelection ? shadowSelection->head : head,
                    replayCbData,
                    passGroupIdx,
                    subIdx,
                    allowAlpha));
            }

            // Per-draw classification for replayed command-buffer draws is handled
            // by the SRV record injected in HookedBuildCommandBuffer. Mark this
            // scope so the lower-level D3D hooks do not overwrite that recorded
            // SRV with the stale immediate-path g_currentDrawTag value.
            struct ReplayScope
            {
                ReplayScope()
                {
                    D3D11Hooks::ResetCommandBufferReplayState();
                    ShaderResources::EnterCommandBufferReplay();
                    PhaseTelemetry::EnterCommandBufferReplay();
                }
                ~ReplayScope()
                {
                    PhaseTelemetry::LeaveCommandBufferReplay();
                    ShaderResources::LeaveCommandBufferReplay();
                    D3D11Hooks::ResetCommandBufferReplayState();
                }
            } replayScope;

            // If an old command buffer lacks our injected record, make it fall
            // back to unknown instead of inheriting the previous replayed draw tag.
            if (EnsureDrawTagWrapperResources() && g_rendererData && g_rendererData->context && g_unknownTagSRV) {
                g_bindingInjectedPixelResources = true;
                g_rendererData->context->PSSetShaderResources(DRAWTAG_SLOT, 1, &g_unknownTagSRV);
                g_bindingInjectedPixelResources = false;
            }

            OriginalRenderCommandBufferPassesImpl(this_, passGroupIdx, replayCbData, subIdx, allowAlpha);
            if (shadowSelection && CurrentShadowSplitPhase() == ShadowSplitPhase::DynamicOverlay) {
                InvalidateCommandBufferFrame(cbData);
            }
        }
    }

    void HookedRenderPersistentPassListImpl(void* persistentPassList, bool allowAlpha)
    {
        ScopedPersistentShadowPassListFilter shadowSplitFilter(persistentPassList);
        if (shadowSplitFilter.Empty() &&
            SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON &&
            ShadowTelemetry::IsShadowCacheActiveForCurrentShadow()) {
            return;
        }

        OriginalRenderPersistentPassListImpl(persistentPassList, allowAlpha);
    }

    void HookedRegisterPassGeometryGroup(void* batchRenderer, BSRenderPassLayout* pass, int group, bool appendOrUnk)
    {
        if (SHADOW_CACHE_DIRECTIONAL_MAPSLOT1_ON && batchRenderer && pass) {
            auto* sourceAccumulator = static_cast<void*>(
                static_cast<std::byte*>(batchRenderer) - kAccumulatorBatchRendererOffset);
            const bool isStaticCaster = IsPrecombineShadowGeometry(pass->geometry);
            if constexpr (kLegacyFinishLevelShadowSplitEnabled) {
                RememberShadowPassClassification(pass, isStaticCaster);
            }
            void* splitBatchRenderer = nullptr;
            if (ShadowTelemetry::RouteShadowCacheRegistration(
                    sourceAccumulator,
                    isStaticCaster,
                    &splitBatchRenderer)) {
                if (splitBatchRenderer) {
                    ShadowTelemetry::NoteShadowCachePassRouted(isStaticCaster);
                    OriginalRegisterPassGeometryGroup(splitBatchRenderer, pass, group, appendOrUnk);
                }
                return;
            }
        }

        OriginalRegisterPassGeometryGroup(batchRenderer, pass, group, appendOrUnk);
    }

    void HookedRenderPassImpl(void* this_, BSRenderPassLayout* head, std::uint32_t techniqueID, bool allowAlpha)
    {
        const unsigned int group = g_renderBatchesGroup;
        ScopedDeferredPrePassDetail detail(
            PhaseTelemetry::DeferredPrePassDetailKind::RenderPassImpl,
            group);
        {
            std::optional<ScopedShadowWork> shadowWork;
            if (IsShadowWorkTelemetryActive()) {
                auto target = MakeShadowPassWorkTarget(
                    this_,
                    head,
                    nullptr,
                    static_cast<std::int32_t>(group),
                    0,
                    allowAlpha);
                target.techniqueID = techniqueID;
                shadowWork.emplace(ShadowTelemetry::WorkKind::ImmediatePass, target);
            }
            ScopedShadowPassChainFilter shadowSplitFilter(head);
            if (auto* filteredHead = shadowSplitFilter.Head()) {
                OriginalRenderPassImpl(this_, filteredHead, techniqueID, allowAlpha);
            }
        }
    }

    REX::W32::ID3D11ShaderResourceView* GetOrCreateStaticDrawTagSRV(const DrawTagClassification& classification)
    {
        if (!g_rendererData) {
            g_rendererData = RE::BSGraphics::GetRendererData();
        }
        if (!g_rendererData || !g_rendererData->device) {
            return g_unknownTagSRV;
        }

        const auto key = MakeStaticDrawTagKey(classification);
        std::lock_guard cacheLock(g_staticDrawTagResourcesMutex);
        if (auto it = g_staticDrawTagResources.find(key); it != g_staticDrawTagResources.end()) {
            return it->second.srv;
        }

        DrawTagData seed{};
        seed.materialTag = classification.materialTag;
        seed.isHead = classification.isHead;
        seed.raceGroupMask = classification.raceGroupMask;
        seed.raceFlags = classification.raceFlags;

        REX::W32::D3D11_BUFFER_DESC desc{};
        desc.usage = REX::W32::D3D11_USAGE_IMMUTABLE;
        desc.byteWidth = sizeof(DrawTagData);
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.cpuAccessFlags = 0;
        desc.miscFlags = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.structureByteStride = sizeof(DrawTagData);

        REX::W32::D3D11_SUBRESOURCE_DATA initialData{};
        initialData.sysMem = &seed;

        StaticDrawTagResource resource{};
        HRESULT hr = g_rendererData->device->CreateBuffer(&desc, &initialData, &resource.buffer);
        if (FAILED(hr)) {
            REX::WARN(
                "GetOrCreateStaticDrawTagSRV: CreateBuffer failed 0x{:08X} tag={} head={} raceMask=0x{:08X} raceFlags=0x{:08X}",
                hr,
                classification.materialTag,
                classification.isHead,
                classification.raceGroupMask,
                classification.raceFlags);
            return g_unknownTagSRV;
        }

        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.format = REX::W32::DXGI_FORMAT_UNKNOWN;
        srvDesc.viewDimension = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.buffer.firstElement = 0;
        srvDesc.buffer.numElements = 1;

        hr = g_rendererData->device->CreateShaderResourceView(resource.buffer, &srvDesc, &resource.srv);
        if (FAILED(hr)) {
            REX::WARN(
                "GetOrCreateStaticDrawTagSRV: CreateSRV failed 0x{:08X} tag={} head={} raceMask=0x{:08X} raceFlags=0x{:08X}",
                hr,
                classification.materialTag,
                classification.isHead,
                classification.raceGroupMask,
                classification.raceFlags);
            resource.buffer->Release();
            return g_unknownTagSRV;
        }

        auto [it, inserted] = g_staticDrawTagResources.emplace(key, resource);
        return it->second.srv;
    }

    // Lazy-create the two immutable structured buffers + SRVs and wire them
    // into the static FakeStructuredResource wrappers. Called from the
    // BuildCommandBuffer hook on first invocation, by which time the
    // device is up.
    bool EnsureDrawTagWrapperResources()
    {
        std::lock_guard initLock(g_tagWrapperInitMutex);
        if (g_tagWrapperResourcesReady) {
            return true;
        }

        if (!g_rendererData) {
            g_rendererData = RE::BSGraphics::GetRendererData();
        }
        if (!g_rendererData || !g_rendererData->device) {
            return false;
        }
        auto* device = g_rendererData->device;

        (void)device;

        g_unknownTagSRV = GetOrCreateStaticDrawTagSRV(MakeUnknownDrawTagClassification());
        g_actorTagSRV = GetOrCreateStaticDrawTagSRV(MakeActorDrawTagClassification(false, 0, 0));
        g_actorHeadTagSRV = GetOrCreateStaticDrawTagSRV(MakeActorDrawTagClassification(true, 0, 0));
        if (!g_unknownTagSRV || !g_actorTagSRV || !g_actorHeadTagSRV) {
            return false;
        }

        g_unknownTagWrapper.srv.store(g_unknownTagSRV, std::memory_order_release);
        g_unknownTagWrapper.fenceCount = 0;
        g_actorTagWrapper.srv.store(g_actorTagSRV, std::memory_order_release);
        g_actorTagWrapper.fenceCount = 0;
        g_actorHeadTagWrapper.srv.store(g_actorHeadTagSRV, std::memory_order_release);
        g_actorHeadTagWrapper.fenceCount = 0;
        g_tagWrapperResourcesReady = true;

        REX::INFO("EnsureDrawTagWrapperResources: wrappers initialized (unknownSRV={}, actorSRV={}, actorHeadSRV={})",
                  static_cast<void*>(g_unknownTagSRV),
                  static_cast<void*>(g_actorTagSRV),
                  static_cast<void*>(g_actorHeadTagSRV));
        ResetCommandBufferDrawTagWrappers();
        return true;
    }

    char* HookedBuildCommandBuffer(void* this_, void* param, void* memoryContext)
    {
        if (!param) {
            return OriginalBuildCommandBuffer(this_, param, memoryContext);
        }

        // Field offsets in BuildCommandBufferParam (verified in IDA):
        //   +0x00  BSGeometry*
        //   +0x14  srvCount   (DWORD)
        //   +0x38  srvSrc ptr (QWORD, points at array of CommandBufferShaderResource)
        auto* paramBytes = reinterpret_cast<char*>(param);
        auto* geom       = *reinterpret_cast<RE::BSGeometry**>(paramBytes);
        auto& srvCount   = *reinterpret_cast<std::uint32_t*>(paramBytes + 0x14);
        auto& srvSrc     = *reinterpret_cast<const CommandBufferShaderResource**>(paramBytes + 0x38);

        std::optional<ScopedShadowWork> shadowWork;
        if (IsShadowWorkTelemetryActive()) {
            ShadowTelemetry::WorkTarget shadowTarget{};
            shadowTarget.owner = this_;
            shadowTarget.geometry = static_cast<void*>(geom);
            shadowTarget.shader = this_;
            shadowTarget.srvCount = srvCount;
            shadowWork.emplace(ShadowTelemetry::WorkKind::BuildCommandBuffer, shadowTarget);
        }

        if (!g_anyReplacementShaderUsesDrawTag.load(std::memory_order_acquire) ||
            !EnsureDrawTagWrapperResources()) {
            return OriginalBuildCommandBuffer(this_, param, memoryContext);
        }

        DrawTagClassification classification{};
        if (geom) {
            std::shared_lock lock(g_actorDrawTaggedGeometryLock);
            if (g_actorDrawTaggedGeometry.contains(geom)) {
                const bool isHead = g_actorHeadDrawTaggedGeometry.contains(geom);
                std::uint32_t raceGroupMask = 0;
                std::uint32_t raceFlags = 0;
                if (auto it = g_actorRaceGroupMaskByGeometry.find(geom);
                    it != g_actorRaceGroupMaskByGeometry.end()) {
                    raceGroupMask = it->second;
                }
                if (auto it = g_actorRaceFlagsByGeometry.find(geom);
                    it != g_actorRaceFlagsByGeometry.end()) {
                    raceFlags = it->second;
                }
                classification = MakeActorDrawTagClassification(isHead, raceGroupMask, raceFlags);
            }
        }

        const std::uint32_t origCount = srvCount;
        const auto* origSrc = srvSrc;
        if (origCount && !origSrc) {
            return OriginalBuildCommandBuffer(this_, param, memoryContext);
        }

        // Fall back if the pass already has more SRVs than our scratch can hold.
        // F4 shaders we've seen have 0?8 SRV records; 32 is generous headroom.
        constexpr std::size_t kMaxSRV = 32;
        if (origCount >= kMaxSRV) {
            return OriginalBuildCommandBuffer(this_, param, memoryContext);
        }

        std::array<CommandBufferShaderResource, kMaxSRV + 1> tempArr{};
        if (origCount && origSrc) {
            std::memcpy(tempArr.data(), origSrc, origCount * sizeof(CommandBufferShaderResource));
        }

        auto& ourRec = tempArr[origCount];
        ourRec.resourceWrapper = static_cast<void*>(GetCommandBufferDrawTagWrapper(geom, classification));
        ourRec.slot  = static_cast<std::uint8_t>(DRAWTAG_SLOT);
        ourRec.stage = 1;  // PS
        ourRec.kind  = 0;  // engine reads SRV from wrapper+0x08

        srvCount = origCount + 1;
        srvSrc   = tempArr.data();

        char* result = OriginalBuildCommandBuffer(this_, param, memoryContext);

        // Restore. The engine memcpy'd our extended array into the cb already,
        // so the temp stack data is no longer referenced after this point.
        srvCount = origCount;
        srvSrc   = origSrc;

        return result;
    }

    RE::NiAVObject* HookedActorLoad3D(RE::TESObjectREFR* ref, bool backgroundLoading)
    {
        auto* loaded3D = OriginalActorLoad3D(ref, backgroundLoading);
        RefreshActorDrawTaggedGeometry(static_cast<RE::Actor*>(ref));
        return loaded3D;
    }

    RE::NiAVObject* HookedPlayerCharacterLoad3D(RE::TESObjectREFR* ref, bool backgroundLoading)
    {
        auto* loaded3D = OriginalPlayerCharacterLoad3D(ref, backgroundLoading);
        RefreshActorDrawTaggedGeometry(static_cast<RE::Actor*>(ref));
        return loaded3D;
    }

    // Set3D ? every "this ref now has a different NiAVObject as its 3D root".
    // Catches paths Load3D doesn't: background-load completions, Update3DModel
    // republishes, FaceGen-driven full swaps, dismemberment.
    void HookedActorSet3D(RE::TESObjectREFR* ref, RE::NiAVObject* a_object, bool a_queue3DTasks)
    {
        OriginalActorSet3D(ref, a_object, a_queue3DTasks);
        RefreshActorDrawTaggedGeometry(static_cast<RE::Actor*>(ref));
    }

    void HookedPlayerCharacterSet3D(RE::TESObjectREFR* ref, RE::NiAVObject* a_object, bool a_queue3DTasks)
    {
        OriginalPlayerCharacterSet3D(ref, a_object, a_queue3DTasks);
        RefreshActorDrawTaggedGeometry(static_cast<RE::Actor*>(ref));
    }

    // OnHeadInitialized ? engine callback fired after BSFaceGenManager finishes
    // building/replacing the face node's children. Body/equipment did not
    // change, so use the head-scoped diff to avoid touching unrelated tags.
    void HookedActorOnHeadInitialized(RE::TESObjectREFR* ref)
    {
        OriginalActorOnHeadInitialized(ref);
        RefreshActorHeadDrawTaggedGeometry(static_cast<RE::Actor*>(ref));
    }

    void HookedPlayerCharacterOnHeadInitialized(RE::TESObjectREFR* ref)
    {
        OriginalPlayerCharacterOnHeadInitialized(ref);
        RefreshActorHeadDrawTaggedGeometry(static_cast<RE::Actor*>(ref));
    }

    // Actor::Update3DModel ? runtime model rebuild. Most LooksMenu/SAF/
    // BodyShapeManager flows reach actor 3D through here; biped attach hooks
    // see only the parts that changed, this hook sees the whole rebuild.
    void HookedUpdate3DModel(void* middleProcess, RE::Actor* actor, bool flag)
    {
        OriginalUpdate3DModel(middleProcess, actor, flag);
        RefreshActorDrawTaggedGeometry(actor);
    }

    // Actor::Reset3D ? script/console-driven reload. Often queues 3D rebuild
    // on the loader thread, so the refresh here may run before the new
    // geometry is published; Set3D / Load3D will catch the late completion.
    void HookedReset3D(RE::Actor* actor, bool a_reloadAll, std::uint32_t a_additionalFlags, bool a_queueReset, std::uint32_t a_excludeFlags)
    {
        OriginalReset3D(actor, a_reloadAll, a_additionalFlags, a_queueReset, a_excludeFlags);
        RefreshActorDrawTaggedGeometry(actor);
    }

}


bool IsPrecombineShadowGeometry_Internal(RE::BSGeometry* geometry)
{
    return IsPrecombineShadowGeometry(geometry);
}

bool IsActorDrawTaggedGeometry_Internal(RE::BSGeometry* geometry)
{
    std::shared_lock lock(g_actorDrawTaggedGeometryLock);
    return g_actorDrawTaggedGeometry.contains(geometry);
}

bool IsRenderBatchesActive_Internal()
{
    return g_renderBatchesDepth != 0;
}

BSRenderPassLayout* GetRenderBatchNodeHead_Internal(void* batchRenderer, int group, unsigned int subIdx)
{
    return GetRenderBatchNodeHeadRaw(GetRenderBatchNodeAddress(batchRenderer, group, subIdx));
}

void ClearActorDrawTaggedGeometry_Internal()
{
    std::unique_lock lock(g_actorDrawTaggedGeometryLock);
    g_actorDrawTaggedGeometry.clear();
    g_actorHeadDrawTaggedGeometry.clear();
    g_actorDrawTaggedGeometryByRef.clear();
    g_actorFaceDrawTaggedGeometryByRef.clear();
    g_actorHeadDrawTaggedGeometryByRef.clear();
    g_actorRaceGroupMaskByGeometry.clear();
    g_actorRaceFlagsByGeometry.clear();
    ResetCommandBufferDrawTagWrappers();
}

void ReleaseDrawTagBuffers_Internal()
{
    {
        std::unique_lock lock(g_commandBufferDrawTagWrappersLock);
        for (auto& [geometry, wrapper] : g_commandBufferDrawTagWrappers) {
            if (wrapper) {
                wrapper->srv.store(nullptr, std::memory_order_release);
            }
        }
        g_commandBufferDrawTagWrappers.clear();
    }
    {
        std::lock_guard lock(g_staticDrawTagResourcesMutex);
        for (auto& [key, resource] : g_staticDrawTagResources) {
            if (resource.srv) {
                resource.srv->Release();
                resource.srv = nullptr;
            }
            if (resource.buffer) {
                resource.buffer->Release();
                resource.buffer = nullptr;
            }
        }
        g_staticDrawTagResources.clear();
    }
    g_unknownTagWrapper.srv.store(nullptr, std::memory_order_release);
    g_actorTagWrapper.srv.store(nullptr, std::memory_order_release);
    g_actorHeadTagWrapper.srv.store(nullptr, std::memory_order_release);
    g_tagWrapperResourcesReady = false;

    g_unknownTagSRV = nullptr;
    if (g_unknownTagBuffer) {
        g_unknownTagBuffer->Release();
        g_unknownTagBuffer = nullptr;
    }
    g_actorTagSRV = nullptr;
    if (g_actorTagBuffer) {
        g_actorTagBuffer->Release();
        g_actorTagBuffer = nullptr;
    }
    g_actorHeadTagSRV = nullptr;
    if (g_actorHeadTagBuffer) {
        g_actorHeadTagBuffer->Release();
        g_actorHeadTagBuffer = nullptr;
    }

    for (auto& srv : g_drawTagRingSRVs) {
        if (srv) {
            srv->Release();
            srv = nullptr;
        }
    }
    for (auto& buffer : g_drawTagRingBuffers) {
        if (buffer) {
            buffer->Release();
            buffer = nullptr;
        }
    }
    g_drawTagSRV = nullptr;
    g_drawTagSRVBuffer = nullptr;
    g_drawTagRingCursor.store(0, std::memory_order_relaxed);
}

static void ReleaseSRVBuffer(REX::W32::ID3D11Buffer*& buffer, REX::W32::ID3D11ShaderResourceView*& srv)
{
    ShaderResources::ReleaseSRVBuffer(buffer, srv);
}
static REX::W32::ID3D11ShaderResourceView* UpdateDrawTagBuffer(
    REX::W32::ID3D11DeviceContext* context,
    float materialTag,
    float isHead,
    std::uint32_t raceGroupMask,
    std::uint32_t raceFlags)
{
    if (!g_rendererData) {
        g_rendererData = RE::BSGraphics::GetRendererData();
        if (!g_rendererData) {
            return nullptr;
        }
    }

    auto* device = g_rendererData->device;
    if (!device) {
        return nullptr;
    }

    const UINT slot = g_drawTagRingCursor.fetch_add(1, std::memory_order_relaxed) % DRAWTAG_RING_SIZE;

    if (!g_drawTagRingBuffers[slot]) {
        REX::W32::D3D11_BUFFER_DESC desc{};
        desc.usage = REX::W32::D3D11_USAGE_DYNAMIC;
        desc.byteWidth = sizeof(DrawTagData);
        desc.bindFlags = REX::W32::D3D11_BIND_SHADER_RESOURCE;
        desc.cpuAccessFlags = REX::W32::D3D11_CPU_ACCESS_WRITE;
        desc.miscFlags = REX::W32::D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.structureByteStride = sizeof(DrawTagData);

        HRESULT hr = device->CreateBuffer(&desc, nullptr, &g_drawTagRingBuffers[slot]);
        if (FAILED(hr)) {
            REX::WARN("UpdateDrawTagBuffer: Failed to create ring buffer slot {}. HRESULT: 0x{:08X}", slot, hr);
            return nullptr;
        }
    }

    if (!g_drawTagRingSRVs[slot]) {
        REX::W32::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.format = REX::W32::DXGI_FORMAT_UNKNOWN;
        srvDesc.viewDimension = REX::W32::D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.buffer.firstElement = 0;
        srvDesc.buffer.numElements = 1;

        HRESULT hr = device->CreateShaderResourceView(g_drawTagRingBuffers[slot], &srvDesc, &g_drawTagRingSRVs[slot]);
        if (FAILED(hr)) {
            REX::WARN("UpdateDrawTagBuffer: Failed to create ring SRV slot {}. HRESULT: 0x{:08X}", slot, hr);
            ReleaseSRVBuffer(g_drawTagRingBuffers[slot], g_drawTagRingSRVs[slot]);
            return nullptr;
        }
    }

    auto* ringBuffer = g_drawTagRingBuffers[slot];
    auto* ringSRV = g_drawTagRingSRVs[slot];

    // Publish the current ring entry as the "global" so paths outside
    // BindDrawTagForCurrentDraw (e.g. MyPSSetShaderResources rebind) still
    // bind a valid SRV. The value they see may be one draw stale, but our
    // per-draw bind below overrides it before the engine's draw runs.
    g_drawTagSRVBuffer = ringBuffer;
    g_drawTagSRV = ringSRV;

    if (!context) {
        return ringSRV;
    }

    // MAP_WRITE_DISCARD on this ring slot's buffer. Since each draw uses a
    // distinct buffer object, the engine's deferred command queue can hold
    // many draws in flight without their SRVs colliding.
    REX::W32::D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(ringBuffer, 0, REX::W32::D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* data = static_cast<DrawTagData*>(mapped.data);
        data->materialTag = materialTag;
        data->isHead = isHead;
        data->raceGroupMask = raceGroupMask;
        data->raceFlags = raceFlags;
        context->Unmap(ringBuffer, 0);
    }

    g_drawTagData.materialTag = materialTag;
    g_drawTagData.isHead = isHead;
    g_drawTagData.raceGroupMask = raceGroupMask;
    g_drawTagData.raceFlags = raceFlags;

    return ringSRV;
}

static void BindDrawTagForCurrentDraw(REX::W32::ID3D11DeviceContext* context, bool force)
{
    if (!context) {
        return;
    }

    if (!force && ShaderResources::IsCommandBufferReplayActive()) {
        return;
    }

    if (!ShaderResources::ActiveReplacementPixelShaderUsesDrawTag()) {
        return;
    }

    auto* drawTagSRV = UpdateDrawTagBuffer(
        context,
        g_currentDrawTag,
        g_currentDrawTagIsHead,
        g_currentDrawTagRaceGroupMask,
        g_currentDrawTagRaceFlags);
    if (!drawTagSRV) {
        return;
    }

    g_bindingInjectedPixelResources = true;
    context->PSSetShaderResources(DRAWTAG_SLOT, 1, &drawTagSRV);
    g_bindingInjectedPixelResources = false;

    if (SHADERENGINE_EFFECTS_ON && ShaderResources::ActiveReplacementPixelShaderNeedsResourceRebind()) {
        ShaderResources::BindInjectedPixelShaderResources(context);
    }

    if (DEBUGGING) {
        REX::W32::ID3D11ShaderResourceView* boundSRV = nullptr;
        context->PSGetShaderResources(DRAWTAG_SLOT, 1, &boundSRV);

        static std::uint32_t checkedDrawTagBinds = 0;
        static std::uint32_t loggedDrawTagBinds = 0;
        ++checkedDrawTagBinds;

        const bool mismatch = boundSRV != drawTagSRV;
        const bool actorTag = g_currentDrawTag >= 0.5f;
        if (loggedDrawTagBinds < 64 || mismatch || actorTag) {
            REX::INFO(
                "DrawTag bind check: draw={}, slot=t{}, tag={}, expectedSRV={}, boundSRV={}, activeReplacement={}, mismatch={}",
                checkedDrawTagBinds,
                DRAWTAG_SLOT,
                g_currentDrawTag,
                static_cast<void*>(drawTagSRV),
                static_cast<void*>(boundSRV),
                ShaderResources::ActiveReplacementPixelShaderActive(),
                mismatch);
            ++loggedDrawTagBinds;
        }

        if (boundSRV) {
            boundSRV->Release();
        }
    }
}



static bool ContainsShaderRegisterToken(const std::string& line, const std::string& token)
{
    std::size_t pos = 0;
    while ((pos = line.find(token, pos)) != std::string::npos) {
        const bool validBefore = pos == 0 || (!std::isalnum(static_cast<unsigned char>(line[pos - 1])) && line[pos - 1] != '_');
        const auto after = pos + token.size();
        const bool validAfter = after >= line.size() || !std::isdigit(static_cast<unsigned char>(line[after]));
        if (validBefore && validAfter) {
            return true;
        }
        ++pos;
    }

    return false;
}


// -- Structs ---

// --- Functions ---

// Engine entry to BSLight::TestFrustumCull. Wraps the original with an
// Enter/Leave pair from LightCullPolicy: Enter optionally scales the BSLight's
// bound radius (geometry[+0x138]) before the engine runs its frustum test,
// Leave restores the source field afterwards. The engine internally caches the
// scaled value into geometry[+0xBC] during its run; Leave deliberately does
// not undo that so downstream consumers of the cached cull sphere see the
// boosted reach. See src/LightCullPolicy.cpp for the rationale.
std::uint32_t HookedBSLightTestFrustumCull(void* light, void* cullingProcess)
{
    const float saved = LightCullPolicy::OnTestFrustumCullEnter(light);
    const auto result = OriginalBSLightTestFrustumCull(light, cullingProcess);
    LightCullPolicy::OnTestFrustumCullLeave(light, saved);
    return result;
}

// Engine entry to BSDFTiledLighting::AddLight. Multiplies the radius arg by
// the current active scale before forwarding. The lambda reads geometry[+0x138]
// for this arg AFTER our cull-side restore has run, so the value it passes us
// is vanilla. Scaling here puts the tiled slot in the same regime as the
// per-volume mesh (which gets its own transient scale via the
// SetupPointLightGeometry hook). Both paths now use the same boosted radius
// so non-shadow lights with both paths active match shadow-light coverage.
void HookedBSDFTiledLightingAddLight(std::uint32_t id,
                                     const RE::NiPoint3* pos, float radius,
                                     const RE::NiColor* color,
                                     const RE::NiPoint3* dir,
                                     bool flagA, bool flagB, bool flagC, bool flagD)
{
    float adjusted = radius;
    const float scale = LightCullPolicy::GetCurrentScale();
    if (std::isfinite(scale) && scale > 0.0f && std::abs(scale - 1.0f) >= 1e-4f) {
        adjusted = radius * scale;
    }
    OriginalBSDFTiledLightingAddLight(id, pos, adjusted, color, dir,
                                      flagA, flagB, flagC, flagD);
}

// Engine entry to BSDFLightShader::SetupPointLightGeometry. Transiently scales
// the light's geometry[+0x138] (source bound radius) for the duration of the
// engine's setup so the world-matrix construction inside
// SetupPointLightTransforms sees the boosted radius and builds an expanded
// volume mesh. Restores immediately on return so shadow projection / fade-
// distance / etc. that read +0x138 outside this window see vanilla.
//
// Helper layout: light[+0xB8] is the geometry pointer; geometry[+0x138] is
// the source bound radius. Same field LightCullPolicy::GetBoundRadiusPtr
// targets. Duplicating the offset math here to avoid widening the public
// LightCullPolicy API just for this one consumer.
bool HookedSetupPointLightGeometry(void* shader, void* light, std::uint32_t mask, char isUnk)
{
    float saved = std::numeric_limits<float>::quiet_NaN();
    float* radiusPtr = nullptr;
    if (light) {
        if (auto* geometry = *reinterpret_cast<void**>(static_cast<std::byte*>(light) + 0xB8)) {
            radiusPtr = reinterpret_cast<float*>(static_cast<std::byte*>(geometry) + 0x138);
            const float scale = LightCullPolicy::GetCurrentScale();
            if (std::isfinite(scale) && scale > 0.0f && std::abs(scale - 1.0f) >= 1e-4f) {
                saved = *radiusPtr;
                *radiusPtr = saved * scale;
            }
        }
    }
    const bool result = OriginalSetupPointLightGeometry(shader, light, mask, isUnk);
    if (!std::isnan(saved) && radiusPtr) {
        *radiusPtr = saved;
    }
    return result;
}

bool InstallDrawTaggingHooks_Internal()
{
    if (OriginalBSLightingShaderSetupGeometry &&
        OriginalBSLightingShaderRestoreGeometry &&
        OriginalBSEffectShaderSetupGeometry &&
        OriginalBSEffectShaderRestoreGeometry &&
        OriginalBSBatchRendererDraw &&
        OriginalBipedAnimAttachSkinnedObject &&
        OriginalBipedAnimAttachBipedWeapon &&
        OriginalBipedAnimAttachToParent &&
        OriginalBipedAnimRemovePart &&
        OriginalActorLoad3D &&
        OriginalPlayerCharacterLoad3D &&
        OriginalActorSet3D &&
        OriginalPlayerCharacterSet3D &&
        OriginalActorOnHeadInitialized &&
        OriginalPlayerCharacterOnHeadInitialized &&
        OriginalUpdate3DModel &&
        OriginalReset3D &&
        OriginalRenderBatches &&
        OriginalRenderGeometryGroup &&
        (!kInstallFinishShadowRenderSplitHooks ||
            REX::FModule::GetRuntimeIndex() != REX::FModule::Runtime::kOG ||
            (OriginalFinishShadowRenderBatches && OriginalFinishShadowRenderGeometryGroup)) &&
        OriginalRenderCommandBufferPassesImpl &&
        OriginalRenderPersistentPassListImpl &&
        (REX::FModule::GetRuntimeIndex() != REX::FModule::Runtime::kOG || OriginalProcessCommandBuffer) &&
        (REX::FModule::GetRuntimeIndex() != REX::FModule::Runtime::kOG || OriginalRegisterPassGeometryGroup) &&
        OriginalRenderPassImpl &&
        OriginalBuildCommandBuffer &&
        OriginalBSLightTestFrustumCull &&
        OriginalBSDFTiledLightingAddLight &&
        OriginalSetupPointLightGeometry) {
        return true;
    }

    if (!OriginalBSLightingShaderSetupGeometry || !OriginalBSLightingShaderRestoreGeometry) {
        REL::Relocation<std::uintptr_t> lightingShaderVTable{ RE::VTABLE::BSLightingShader[0] };
        if (!OriginalBSLightingShaderSetupGeometry) {
            OriginalBSLightingShaderSetupGeometry = reinterpret_cast<BSShaderSetupGeometry_t>(
                lightingShaderVTable.write_vfunc(7, reinterpret_cast<void*>(&HookedBSLightingShaderSetupGeometry)));

            if (!OriginalBSLightingShaderSetupGeometry) {
                REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSLightingShader::SetupGeometry hook");
                return false;
            }

            REX::INFO("InstallDrawTaggingHooks_Internal: BSLightingShader::SetupGeometry hook installed");
        }

        if (!OriginalBSLightingShaderRestoreGeometry) {
            OriginalBSLightingShaderRestoreGeometry = reinterpret_cast<BSShaderRestoreGeometry_t>(
                lightingShaderVTable.write_vfunc(8, reinterpret_cast<void*>(&HookedBSLightingShaderRestoreGeometry)));

            if (!OriginalBSLightingShaderRestoreGeometry) {
                REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSLightingShader::RestoreGeometry hook");
                return false;
            }

            REX::INFO("InstallDrawTaggingHooks_Internal: BSLightingShader::RestoreGeometry hook installed");
        }
    }

    if (!OriginalBSEffectShaderSetupGeometry || !OriginalBSEffectShaderRestoreGeometry) {
        REL::Relocation<std::uintptr_t> effectShaderVTable{ RE::VTABLE::BSEffectShader[0] };
        if (!OriginalBSEffectShaderSetupGeometry) {
            OriginalBSEffectShaderSetupGeometry = reinterpret_cast<BSShaderSetupGeometry_t>(
                effectShaderVTable.write_vfunc(7, reinterpret_cast<void*>(&HookedBSEffectShaderSetupGeometry)));

            if (!OriginalBSEffectShaderSetupGeometry) {
                REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSEffectShader::SetupGeometry hook");
                return false;
            }

            REX::INFO("InstallDrawTaggingHooks_Internal: BSEffectShader::SetupGeometry hook installed");
        }

        if (!OriginalBSEffectShaderRestoreGeometry) {
            OriginalBSEffectShaderRestoreGeometry = reinterpret_cast<BSShaderRestoreGeometry_t>(
                effectShaderVTable.write_vfunc(8, reinterpret_cast<void*>(&HookedBSEffectShaderRestoreGeometry)));

            if (!OriginalBSEffectShaderRestoreGeometry) {
                REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSEffectShader::RestoreGeometry hook");
                return false;
            }

            REX::INFO("InstallDrawTaggingHooks_Internal: BSEffectShader::RestoreGeometry hook installed");
        }
    }

    if (!OriginalBSBatchRendererDraw) {
        // IDA OG 1.10.163:
        //   BSBatchRenderer::Draw @ 0x14287EDE0, REL ID 1152191
        // ABI: void __fastcall(pass=RCX, arg2=RDX, arg3=R8,
        //                      dynamicDrawData=R9)
        // The first 15 bytes end on an instruction boundary:
        //   40 53                push rbx
        //   41 56                push r14
        //   48 83 EC 58          sub rsp,58h
        //   48 8B 59 18          mov rbx,[rcx+18h]
        //   4C 8B F1             mov r14,rcx
        // The existing AE relocation is retained for draw tagging, but the
        // interior shadow replay is OG-only because its enclosing
        // DeferredLightsImpl contract is not installed on AE.
        if (REX::FModule::IsRuntimeOG()) {
            constexpr std::array<std::uint8_t, 15> kExpectedDrawPrologue{
                0x40, 0x53, 0x41, 0x56, 0x48,
                0x83, 0xEC, 0x58, 0x48, 0x8B,
                0x59, 0x18, 0x4C, 0x8B, 0xF1
            };
            const auto address = Hooks::Addresses::BSBatchRendererDraw.address();
            if (std::memcmp(
                    reinterpret_cast<const void*>(address),
                    kExpectedDrawPrologue.data(),
                    kExpectedDrawPrologue.size()) != 0) {
                REX::WARN(
                    "InstallDrawTaggingHooks_Internal: BSBatchRenderer::Draw "
                    "@ {:#x} failed expected-byte validation; hook not "
                    "installed",
                    address);
                return false;
            }
        }
        constexpr std::size_t kDrawPrologueSize = 15;
        OriginalBSBatchRendererDraw = Hooks::CreateBranchGateway5<BSBatchRendererDraw_t>(Hooks::Addresses::BSBatchRendererDraw, kDrawPrologueSize, reinterpret_cast<void*>(&HookedBSBatchRendererDraw));

        if (!OriginalBSBatchRendererDraw) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSBatchRenderer::Draw hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSBatchRenderer::Draw hook installed");
    }

    if (!OriginalRenderBatches) {
        // IDA OG 0x14287F380: 5 + 4 + 1 + 4 bytes reaches a clean boundary
        // after `sub rsp, 60h`, exactly enough for the E9-5 patch.
        constexpr std::size_t kRenderBatchesPrologueSize = 14;
        OriginalRenderBatches = Hooks::CreateBranchGateway5<RenderBatches_t>(
            Hooks::Addresses::RenderBatches,
            kRenderBatchesPrologueSize,
            reinterpret_cast<void*>(&HookedRenderBatches));

        if (!OriginalRenderBatches) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSBatchRenderer::RenderBatches hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSBatchRenderer::RenderBatches hook installed");
    }

    if (!OriginalRenderGeometryGroup) {
        // OG ID 1379976, 0x14282F110. IDA-verified clean boundary:
        // mov [rsp+10],rbp (5) + mov [rsp+18],rsi (5) + push rdi (1) +
        // sub rsp,20h (4) = 15 bytes. 14 splits the sub instruction.
        constexpr std::size_t kRenderGeometryGroupPrologueSize = 15;
        OriginalRenderGeometryGroup = Hooks::CreateBranchGateway5<RenderGeometryGroup_t>(
            Hooks::Addresses::RenderGeometryGroup,
            kRenderGeometryGroupPrologueSize,
            reinterpret_cast<void*>(&HookedRenderGeometryGroup));

        if (!OriginalRenderGeometryGroup) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSShaderAccumulator::RenderGeometryGroup hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSShaderAccumulator::RenderGeometryGroup hook installed");
    }

    auto verifyRel32Call = [](const char* label,
                              std::uintptr_t callSite,
                              std::uintptr_t expectedTarget) -> bool {

        auto* callBytes = reinterpret_cast<const std::uint8_t*>(callSite);
        if (callBytes[0] != 0xE8) {
            REX::WARN("InstallDrawTaggingHooks_Internal: {} call site at {:#x} is not a CALL rel32",
                      label,
                      callSite);
            return false;
        }

        std::int32_t rel32 = 0;
        std::memcpy(&rel32, callBytes + 1, sizeof(rel32));
        const std::uintptr_t actualTarget = callSite + 5 + rel32;
        if (actualTarget != expectedTarget) {
            REX::WARN("InstallDrawTaggingHooks_Internal: {} call site {:#x} targets {:#x}, expected {:#x}",
                      label,
                      callSite,
                      actualTarget,
                      expectedTarget);
            return false;
        }

        return true;
    };

    auto patchVerifiedCall = [&verifyRel32Call](const char* label,
                                                std::uintptr_t callSite,
                                                std::uintptr_t expectedTarget,
                                                void* hook,
                                                std::uintptr_t* outOriginal) -> bool {
        if (!verifyRel32Call(label, callSite, expectedTarget)) {
            return false;
        }

        REL::Relocation<std::uintptr_t> callSiteRel{ callSite };
        const auto original = callSiteRel.write_call<5>(reinterpret_cast<std::uintptr_t>(hook));
        if (outOriginal) {
            *outOriginal = original;
        }
        REX::INFO("InstallDrawTaggingHooks_Internal: {} call-site redirect installed at {:#x}",
                  label,
                  callSite);
        return true;
    };

    if (kInstallFinishShadowRenderSplitHooks &&
        REX::FModule::GetRuntimeIndex() == REX::FModule::Runtime::kOG &&
        (!OriginalFinishShadowRenderBatches || !OriginalFinishShadowRenderGeometryGroup)) {

        const auto finishShadowRenderBatchesCallSite =
            Hooks::Addresses::FinishAccumulatingShadowMapOrMask.address() + Hooks::Offsets::FinishShadowRenderBatchesCallOG;
        const auto finishShadowRenderGeometryGroupCallSite =
            Hooks::Addresses::FinishAccumulatingShadowMapOrMask.address() + Hooks::Offsets::FinishShadowRenderGeometryGroupCallOG;
        if (!OriginalFinishShadowRenderBatches &&
            !verifyRel32Call(
                "FinishAccumulating_ShadowMapOrMask -> RenderBatches",
                finishShadowRenderBatchesCallSite,
                Hooks::Addresses::RenderBatches.address())) {
            return false;
        }
        if (!OriginalFinishShadowRenderGeometryGroup &&
            !verifyRel32Call(
                "FinishAccumulating_ShadowMapOrMask -> RenderGeometryGroup",
                finishShadowRenderGeometryGroupCallSite,
                Hooks::Addresses::RenderGeometryGroup.address())) {
            return false;
        }

        if (!OriginalFinishShadowRenderBatches) {
            std::uintptr_t original = 0;
            if (!patchVerifiedCall(
                    "FinishAccumulating_ShadowMapOrMask -> RenderBatches",
                    finishShadowRenderBatchesCallSite,
                    Hooks::Addresses::RenderBatches.address(),
                    reinterpret_cast<void*>(&HookedFinishShadowRenderBatches),
                    &original)) {
                return false;
            }
            OriginalFinishShadowRenderBatches = reinterpret_cast<RenderBatches_t>(original);
        }

        if (!OriginalFinishShadowRenderGeometryGroup) {
            std::uintptr_t original = 0;
            if (!patchVerifiedCall(
                    "FinishAccumulating_ShadowMapOrMask -> RenderGeometryGroup",
                    finishShadowRenderGeometryGroupCallSite,
                    Hooks::Addresses::RenderGeometryGroup.address(),
                    reinterpret_cast<void*>(&HookedFinishShadowRenderGeometryGroup),
                    &original)) {
                return false;
            }
            OriginalFinishShadowRenderGeometryGroup = reinterpret_cast<RenderGeometryGroup_t>(original);
        }
    }

    if (!OriginalRenderCommandBufferPassesImpl) {
        constexpr std::size_t kRenderCmdBufPrologueSize = 15;
        OriginalRenderCommandBufferPassesImpl = Hooks::CreateBranchGateway5<RenderCommandBufferPassesImpl_t>(Hooks::Addresses::RenderCommandBufferPassesImpl, kRenderCmdBufPrologueSize, reinterpret_cast<void*>(&HookedRenderCommandBufferPassesImpl));

        if (!OriginalRenderCommandBufferPassesImpl) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSBatchRenderer::RenderCommandBufferPassesImpl hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSBatchRenderer::RenderCommandBufferPassesImpl hook installed");
    }

    if (!OriginalRenderPersistentPassListImpl) {

        // Avoid an entry detour here. Ghidra shows the shadow group-9 path as:
        //   BSBatchRenderer::GeometryGroup::Render + call 0x719
        //     -> RenderPersistentPassListImpl(PersistentPassList&, bool)
        // Patch that direct call only, and verify the rel32 target first so a
        // wrong runtime/layout fails closed instead of corrupting the prologue.
        const std::uintptr_t target = Hooks::Addresses::RenderPersistentPassListImpl.address();
        const std::uintptr_t callSite = target + Hooks::Offsets::GeometryGroupRenderPersistentPassListCallOG;
        auto* callBytes = reinterpret_cast<const std::uint8_t*>(callSite);
        if (callBytes[0] != 0xE8) {
            REX::WARN("InstallDrawTaggingHooks_Internal: GeometryGroup::Render persistent-list call site at {:#x} is not a CALL rel32", callSite);
            return false;
        }

        std::int32_t rel32 = 0;
        std::memcpy(&rel32, callBytes + 1, sizeof(rel32));
        const std::uintptr_t actualTarget = callSite + 5 + rel32;
        if (actualTarget != target) {
            REX::WARN(
                "InstallDrawTaggingHooks_Internal: GeometryGroup::Render persistent-list call site {:#x} targets {:#x}, expected {:#x}",
                callSite,
                actualTarget,
                target);
            return false;
        }

        REL::Relocation<std::uintptr_t> callSiteRel{ callSite };
        OriginalRenderPersistentPassListImpl = reinterpret_cast<RenderPersistentPassListImpl_t>(
            callSiteRel.write_call<5>(reinterpret_cast<std::uintptr_t>(&HookedRenderPersistentPassListImpl)));

        if (!OriginalRenderPersistentPassListImpl) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to redirect GeometryGroup::Render persistent-list call site at {:#x}", callSite);
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: GeometryGroup::Render persistent-list call-site redirect installed at {:#x}", callSite);
    }

    if (REX::FModule::GetRuntimeIndex() == REX::FModule::Runtime::kOG && !OriginalProcessCommandBuffer) {
        // 1.10.163 version file: ID 673619 -> 0x141D13A10.
        // Prologue is three 5-byte non-RIP stack spills, so 15 bytes is a
        // clean gateway boundary for the 14-byte absolute jump patch.
        constexpr std::size_t kProcessCommandBufferPrologueSize = 15;
        OriginalProcessCommandBuffer = Hooks::CreateBranchGateway5<ProcessCommandBuffer_t>(
            Hooks::Addresses::ProcessCommandBuffer,
            kProcessCommandBufferPrologueSize,
            reinterpret_cast<void*>(&HookedProcessCommandBuffer));

        if (!OriginalProcessCommandBuffer) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSGraphics::Renderer::ProcessCommandBuffer hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSGraphics::Renderer::ProcessCommandBuffer hook installed");
    }

    if (REX::FModule::GetRuntimeIndex() == REX::FModule::Runtime::kOG && !OriginalRegisterPassGeometryGroup) {

        const std::uintptr_t callSite =
            Hooks::Addresses::RegisterObjectShadowMapOrMask.address() + Hooks::Offsets::RegisterObjectShadowMapOrMaskRegisterPassCallOG;
        auto* callBytes = reinterpret_cast<const std::uint8_t*>(callSite);
        if (callBytes[0] != 0xE8) {
            REX::WARN("InstallDrawTaggingHooks_Internal: RegisterObject_ShadowMapOrMask RegisterPass call site at {:#x} is not a CALL rel32", callSite);
            return false;
        }

        std::int32_t rel32 = 0;
        std::memcpy(&rel32, callBytes + 1, sizeof(rel32));
        const std::uintptr_t actualTarget = callSite + 5 + rel32;
        const std::uintptr_t expectedTarget = Hooks::Addresses::RegisterPassGeometryGroup.address();
        if (actualTarget != expectedTarget) {
            REX::WARN(
                "InstallDrawTaggingHooks_Internal: RegisterObject_ShadowMapOrMask RegisterPass call site {:#x} targets {:#x}, expected {:#x}",
                callSite,
                actualTarget,
                expectedTarget);
            return false;
        }

        REL::Relocation<std::uintptr_t> callSiteRel{ callSite };
        OriginalRegisterPassGeometryGroup = reinterpret_cast<RegisterPassGeometryGroup_t>(
            callSiteRel.write_call<5>(reinterpret_cast<std::uintptr_t>(&HookedRegisterPassGeometryGroup)));

        if (!OriginalRegisterPassGeometryGroup) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to redirect RegisterObject_ShadowMapOrMask RegisterPass call site at {:#x}", callSite);
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: RegisterObject_ShadowMapOrMask RegisterPass call-site redirect installed at {:#x}", callSite);
    }

    if (!OriginalRenderPassImpl) {
        // IDA OG 0x142880030: clean 16-byte boundary after
        // `mov rbx, rdx`; the first 14 bytes would split that instruction.
        constexpr std::size_t kRenderPassImplPrologueSize = 16;
        OriginalRenderPassImpl = Hooks::CreateBranchGateway5<RenderPassImpl_t>(
            Hooks::Addresses::RenderPassImpl,
            kRenderPassImplPrologueSize,
            reinterpret_cast<void*>(&HookedRenderPassImpl));

        if (!OriginalRenderPassImpl) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSBatchRenderer::RenderPassImpl hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSBatchRenderer::RenderPassImpl hook installed");
    }

    if (!OriginalBuildCommandBuffer) {
        // Prologue: mov [rsp+arg_0], rbx; push rbp/rsi/rdi/r12/r13/r14/r15
        // 5 + 1 + 1 + 1 + 2 + 2 + 2 + 2 = 16 bytes (clean instruction boundary).
        constexpr std::size_t kBuildCommandBufferPrologueSize = 16;
        OriginalBuildCommandBuffer = Hooks::CreateBranchGateway5<BuildCommandBuffer_t>(Hooks::Addresses::BuildCommandBuffer, kBuildCommandBufferPrologueSize, reinterpret_cast<void*>(&HookedBuildCommandBuffer));

        if (!OriginalBuildCommandBuffer) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSShader::BuildCommandBuffer hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSShader::BuildCommandBuffer hook installed");
    }

    if (!OriginalActorLoad3D) {
        REL::Relocation<std::uintptr_t> actorVTable{ RE::VTABLE::Actor[0] };
        OriginalActorLoad3D = reinterpret_cast<ActorLoad3D_t>(
            actorVTable.write_vfunc(0x86, reinterpret_cast<void*>(&HookedActorLoad3D)));

        if (!OriginalActorLoad3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install Actor::Load3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: Actor::Load3D hook installed");
    }

    if (!OriginalPlayerCharacterLoad3D) {
        REL::Relocation<std::uintptr_t> playerCharacterVTable{ RE::VTABLE::PlayerCharacter[0] };
        OriginalPlayerCharacterLoad3D = reinterpret_cast<ActorLoad3D_t>(
            playerCharacterVTable.write_vfunc(0x86, reinterpret_cast<void*>(&HookedPlayerCharacterLoad3D)));

        if (!OriginalPlayerCharacterLoad3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install PlayerCharacter::Load3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: PlayerCharacter::Load3D hook installed");
    }

    if (!OriginalBipedAnimAttachSkinnedObject) {
        constexpr std::size_t kAttachSkinnedObjectPrologueSize = 15;
        OriginalBipedAnimAttachSkinnedObject = Hooks::CreateBranchGateway5<BipedAnimAttachSkinnedObject_t>(Hooks::Addresses::BipedAnimAttachSkinnedObject, kAttachSkinnedObjectPrologueSize, reinterpret_cast<void*>(&HookedBipedAnimAttachSkinnedObject));

        if (!OriginalBipedAnimAttachSkinnedObject) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BipedAnim::AttachSkinnedObject hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BipedAnim::AttachSkinnedObject hook installed");
    }

    if (!OriginalBipedAnimAttachBipedWeapon) {
        constexpr std::size_t kAttachBipedWeaponPrologueSize = 18;
        OriginalBipedAnimAttachBipedWeapon = Hooks::CreateBranchGateway5<BipedAnimAttachBipedWeapon_t>(Hooks::Addresses::BipedAnimAttachBipedWeapon, kAttachBipedWeaponPrologueSize, reinterpret_cast<void*>(&HookedBipedAnimAttachBipedWeapon));

        if (!OriginalBipedAnimAttachBipedWeapon) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BipedAnim::AttachBipedWeapon hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BipedAnim::AttachBipedWeapon hook installed");
    }

    if (!OriginalBipedAnimAttachToParent) {
        constexpr std::size_t kAttachToParentPrologueSize = 15;
        OriginalBipedAnimAttachToParent = Hooks::CreateBranchGateway5<BipedAnimAttachToParent_t>(Hooks::Addresses::BipedAnimAttachToParent, kAttachToParentPrologueSize, reinterpret_cast<void*>(&HookedBipedAnimAttachToParent));

        if (!OriginalBipedAnimAttachToParent) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BipedAnim::AttachToParent hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BipedAnim::AttachToParent hook installed");
    }

    if (!OriginalBipedAnimRemovePart) {
        constexpr std::size_t kRemovePartPrologueSize = 15;
        OriginalBipedAnimRemovePart = Hooks::CreateBranchGateway5<BipedAnimRemovePart_t>(Hooks::Addresses::BipedAnimRemovePart, kRemovePartPrologueSize, reinterpret_cast<void*>(&HookedBipedAnimRemovePart));

        if (!OriginalBipedAnimRemovePart) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BipedAnim::RemovePart hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BipedAnim::RemovePart hook installed");
    }

    if (!OriginalActorSet3D) {
        REL::Relocation<std::uintptr_t> actorVTable{ RE::VTABLE::Actor[0] };
        OriginalActorSet3D = reinterpret_cast<Set3D_t>(
            actorVTable.write_vfunc(0x88, reinterpret_cast<void*>(&HookedActorSet3D)));

        if (!OriginalActorSet3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install Actor::Set3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: Actor::Set3D hook installed");
    }

    if (!OriginalPlayerCharacterSet3D) {
        REL::Relocation<std::uintptr_t> playerCharacterVTable{ RE::VTABLE::PlayerCharacter[0] };
        OriginalPlayerCharacterSet3D = reinterpret_cast<Set3D_t>(
            playerCharacterVTable.write_vfunc(0x88, reinterpret_cast<void*>(&HookedPlayerCharacterSet3D)));

        if (!OriginalPlayerCharacterSet3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install PlayerCharacter::Set3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: PlayerCharacter::Set3D hook installed");
    }

    if (!OriginalActorOnHeadInitialized) {
        REL::Relocation<std::uintptr_t> actorVTable{ RE::VTABLE::Actor[0] };
        OriginalActorOnHeadInitialized = reinterpret_cast<OnHeadInitialized_t>(
            actorVTable.write_vfunc(0x98, reinterpret_cast<void*>(&HookedActorOnHeadInitialized)));

        if (!OriginalActorOnHeadInitialized) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install Actor::OnHeadInitialized hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: Actor::OnHeadInitialized hook installed");
    }

    if (!OriginalPlayerCharacterOnHeadInitialized) {
        REL::Relocation<std::uintptr_t> playerCharacterVTable{ RE::VTABLE::PlayerCharacter[0] };
        OriginalPlayerCharacterOnHeadInitialized = reinterpret_cast<OnHeadInitialized_t>(
            playerCharacterVTable.write_vfunc(0x98, reinterpret_cast<void*>(&HookedPlayerCharacterOnHeadInitialized)));

        if (!OriginalPlayerCharacterOnHeadInitialized) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install PlayerCharacter::OnHeadInitialized hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: PlayerCharacter::OnHeadInitialized hook installed");
    }

    if (!OriginalUpdate3DModel) {
        // 5-byte patch is mandatory here for BodyShapeManager coexistence.
        // BSM hooks AIProcess::Update3dModel with its own Write5Branch over
        // the 5-byte first instruction (mov [rsp+0x18], rbp). If we used a
        // 14-byte absolute patch, we'd overwrite bytes 0-13 ? and BSM's gateway
        // hard-codes a JMP to (target+5), which would land in the middle of
        // our patch and execute garbage (observed crash: EXCEPTION_ACCESS_
        // VIOLATION jumping to a bogus RIP). With CreateBranchGateway5 we
        // only touch bytes 0-4, leaving the original `push rsi` at byte 5
        // intact for BSM's gateway to land on.
        //
        // Prologue (verified by reading Fallout4.exe at 0x140E3C9C0):
        //   48 89 6C 24 18      mov [rsp+0x18], rbp     ; 5 bytes
        constexpr std::size_t kUpdate3DModelPrologueSize = 5;
        OriginalUpdate3DModel = Hooks::CreateBranchGateway5<Update3DModel_t>(
            Hooks::Addresses::Update3DModel, kUpdate3DModelPrologueSize, reinterpret_cast<void*>(&HookedUpdate3DModel));

        if (!OriginalUpdate3DModel) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install AIProcess::Update3dModel hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: AIProcess::Update3dModel hook installed");
    }

    if (!OriginalReset3D) {
        // BSM doesn't hook Reset3D, so coexistence isn't a concern here, but
        // we keep the 5-byte path for consistency and to leave room for other
        // mods that might patch the same function.
        //
        // Prologue (verified by reading Fallout4.exe at 0x140D8A1F0):
        //   48 89 5C 24 08      mov [rsp+0x08], rbx     ; 5 bytes
        constexpr std::size_t kReset3DPrologueSize = 5;
        OriginalReset3D = Hooks::CreateBranchGateway5<Reset3D_t>(
            Hooks::Addresses::Reset3D, kReset3DPrologueSize, reinterpret_cast<void*>(&HookedReset3D));

        if (!OriginalReset3D) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install Actor::Reset3D hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: Actor::Reset3D hook installed");
    }

    if (!OriginalBSLightTestFrustumCull) {
        // Prologue at OG 0x14285ACB0 (verified by IDA disasm) ? 14 bytes ends
        // on a clean instruction boundary right before `sub rsp, 70h`:
        //   mov [rsp+0x10], rbx     ; 5 bytes
        //   mov [rsp+0x18], rbp     ; 5 bytes
        //   push rsi                 ; 1 byte
        //   push rdi                 ; 1 byte
        //   push r14                 ; 2 bytes
        // Total = 14; CreateBranchGateway5 still only writes a 5-byte patch.
        constexpr std::size_t kBSLightTestFrustumCullPrologueSize = 14;
        OriginalBSLightTestFrustumCull = Hooks::CreateBranchGateway5<BSLightTestFrustumCull_t>(
            Hooks::Addresses::BSLightTestFrustumCull, kBSLightTestFrustumCullPrologueSize,
            reinterpret_cast<void*>(&HookedBSLightTestFrustumCull));

        if (!OriginalBSLightTestFrustumCull) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSLight::TestFrustumCull hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSLight::TestFrustumCull hook installed");
    }

    if (REX::FModule::IsRuntimeOG() && !OriginalBSDFTiledLightingAddLight) {
        // IDA OG 1.10.163:
        //   TryAddTiledLightLambda @ 0x142813CE0, REL ID 999390
        //   call site +0x281 @ 0x142813F61
        //   E8 6A 9C 05 00 -> BSDFTiledLighting::AddLight @ 0x14286DBD0
        // ABI: RCX=id, RDX=NiPoint3* NDC position, XMM2=radius,
        // R9=NiColor* linear color; the cb2[3] attenuation vector and four
        // bools are stack args.
        // AE is unconfirmed and deliberately not patched.
        constexpr std::array<std::uint8_t, 5> kExpectedAddLightCall{
            0xE8, 0x6A, 0x9C, 0x05, 0x00
        };
        const std::uintptr_t callSite = Hooks::Addresses::TryAddTiledLightLambda.address() + Hooks::Offsets::AddLightCallSiteOG;
        if (std::memcmp(
                reinterpret_cast<const void*>(callSite),
                kExpectedAddLightCall.data(),
                kExpectedAddLightCall.size()) != 0) {
            REX::WARN(
                "InstallDrawTaggingHooks_Internal: AddLight call site {:#x} "
                "failed expected-byte validation; redirect not installed",
                callSite);
            return false;
        }
        REL::Relocation<std::uintptr_t> callSiteRel{ callSite };
        OriginalBSDFTiledLightingAddLight = reinterpret_cast<BSDFTiledLightingAddLight_t>(
            callSiteRel.write_call<5>(reinterpret_cast<std::uintptr_t>(&HookedBSDFTiledLightingAddLight)));

        if (!OriginalBSDFTiledLightingAddLight) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to redirect BSDFTiledLighting::AddLight call site at {:#x}", callSite);
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSDFTiledLighting::AddLight call-site redirect installed at {:#x} (scales radius arg)", callSite);
    }

    if (REX::FModule::IsRuntimeOG() && !OriginalSetupPointLightGeometry) {
        // IDA OG 1.10.163, REL ID 212931, 0x1428C37A0:
        //   44 88 4C 24 20   mov [rsp+20h],r9b
        // ABI: bool __fastcall(BSDFLightShader*=RCX, BSLight*=RDX,
        //                      uint32_t=R8D, char=R9B)
        // The instruction is exactly five bytes and is the complete safe
        // prologue copied by the gateway. AE is unconfirmed.
        constexpr std::array<std::uint8_t, 5> kExpectedSetupPointPrologue{
            0x44, 0x88, 0x4C, 0x24, 0x20
        };
        const auto setupAddress =
            Hooks::Addresses::SetupPointLightGeometry.address();
        if (std::memcmp(
                reinterpret_cast<const void*>(setupAddress),
                kExpectedSetupPointPrologue.data(),
                kExpectedSetupPointPrologue.size()) != 0) {
            REX::WARN(
                "InstallDrawTaggingHooks_Internal: SetupPointLightGeometry "
                "@ {:#x} failed expected-byte validation; hook not installed",
                setupAddress);
            return false;
        }
        constexpr std::size_t kSetupPointLightGeometryPrologueSize = 5;
        OriginalSetupPointLightGeometry = Hooks::CreateBranchGateway5<SetupPointLightGeometry_t>(
            Hooks::Addresses::SetupPointLightGeometry, kSetupPointLightGeometryPrologueSize,
            reinterpret_cast<void*>(&HookedSetupPointLightGeometry));

        if (!OriginalSetupPointLightGeometry) {
            REX::WARN("InstallDrawTaggingHooks_Internal: Failed to install BSDFLightShader::SetupPointLightGeometry hook");
            return false;
        }

        REX::INFO("InstallDrawTaggingHooks_Internal: BSDFLightShader::SetupPointLightGeometry hook installed");
    }

    return true;
}
