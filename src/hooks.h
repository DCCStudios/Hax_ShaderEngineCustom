#pragma once

#include <PCH.h>

namespace Hooks
{
    namespace Addresses
    {
        extern REL::Relocation<std::uintptr_t> D3D11CreateDeviceAndSwapChainCall;
        extern REL::Relocation<std::uintptr_t> ClipCursor;

        extern REL::Relocation<std::uintptr_t> BSBatchRendererDraw;
        extern REL::Relocation<std::uintptr_t> BipedAnimAttachSkinnedObject;
        extern REL::Relocation<std::uintptr_t> BipedAnimAttachBipedWeapon;
        extern REL::Relocation<std::uintptr_t> BipedAnimAttachToParent;
        extern REL::Relocation<std::uintptr_t> BipedAnimRemovePart;
        extern REL::Relocation<std::uintptr_t> Update3DModel;
        extern REL::Relocation<std::uintptr_t> Reset3D;
        extern REL::Relocation<std::uintptr_t> RenderBatches;
        extern REL::Relocation<std::uintptr_t> RenderGeometryGroup;
        extern REL::Relocation<std::uintptr_t> FinishAccumulatingShadowMapOrMask;
        extern REL::Relocation<std::uintptr_t> RenderCommandBufferPassesImpl;
        extern REL::Relocation<std::uintptr_t> RenderPersistentPassListImpl;
        extern REL::Relocation<std::uintptr_t> ProcessCommandBuffer;
        extern REL::Relocation<std::uintptr_t> RenderPassImpl;
        extern REL::Relocation<std::uintptr_t> RegisterObjectShadowMapOrMask;
        extern REL::Relocation<std::uintptr_t> RegisterPassGeometryGroup;
        extern REL::Relocation<std::uintptr_t> BuildCommandBuffer;
        extern REL::Relocation<std::uintptr_t> BSLightTestFrustumCull;
        extern REL::Relocation<std::uintptr_t> TryAddTiledLightLambda;
        extern REL::Relocation<std::uintptr_t> SetupPointLightGeometry;
    }

    namespace Offsets
    {
        inline constexpr std::uint32_t D3D11CreateDeviceAndSwapChainCallOG = 0x419;
        inline constexpr std::uint32_t D3D11CreateDeviceAndSwapChainCallAE = 0x410;

        inline constexpr std::uintptr_t FinishShadowRenderBatchesCallOG = 0x38;       // 0x14282E4A8 - 0x14282E470
        inline constexpr std::uintptr_t FinishShadowRenderGeometryGroupCallOG = 0x51; // 0x14282E4C1 - 0x14282E470
        inline constexpr std::uintptr_t GeometryGroupRenderPersistentPassListCallOG = 0x719; // 0x142880909 - 0x1428801F0
        inline constexpr std::uintptr_t RegisterObjectShadowMapOrMaskRegisterPassCallOG = 0xCF; // 0x14282D8CF - 0x14282D800
        inline constexpr std::uintptr_t AddLightCallSiteOG = 0x281; // 0x142813F61 - 0x142813CE0
    }

    bool PatchVTableSlot(void** slot, void* hook);

    template <class OriginalFn>
    bool InstallVTableSlot(void** vtable, std::size_t index, void* hook, OriginalFn& original)
    {
        void** slot = &vtable[index];
        original = reinterpret_cast<OriginalFn>(*slot);
        return PatchVTableSlot(slot, hook);
    }

    template <class OriginalFn>
    void EnsureVTableSlot(void** vtable, std::size_t index, void* hook, OriginalFn& original)
    {
        void** slot = &vtable[index];
        void* current = *slot;
        if (current == hook || current != reinterpret_cast<void*>(original)) {
            return;
        }

        PatchVTableSlot(slot, hook);
    }

    template <class T>
    T CreateBranchGateway5(REL::Relocation<std::uintptr_t>& target, std::size_t prologueSize, void* hook)
    {
        const auto targetAddress = target.address();
        auto& trampoline = REL::GetTrampoline();
        auto* gateway = static_cast<std::byte*>(trampoline.allocate(prologueSize + sizeof(REL::ASM::JMP14)));
        std::memcpy(gateway, reinterpret_cast<void*>(targetAddress), prologueSize);

        if (prologueSize >= 5 && gateway[0] == std::byte{ 0xE9 }) {
            std::int32_t oldRel32;
            std::memcpy(&oldRel32, gateway + 1, sizeof(oldRel32));
            const auto absoluteDest = static_cast<std::int64_t>(targetAddress) + 5 + oldRel32;
            const auto newRel64 = absoluteDest - (reinterpret_cast<std::int64_t>(gateway) + 5);
            if (newRel64 < INT32_MIN || newRel64 > INT32_MAX) {
                REX::WARN(
                    "CreateBranchGateway5: captured JMP destination {:#x} is unreachable from gateway {} via rel32 - chained hook likely broken",
                    static_cast<std::uintptr_t>(absoluteDest),
                    static_cast<void*>(gateway));
                return nullptr;
            }
            const auto newRel32 = static_cast<std::int32_t>(newRel64);
            std::memcpy(gateway + 1, &newRel32, sizeof(newRel32));
            REX::INFO(
                "CreateBranchGateway5: target {:#x} already hooked - re-encoded captured E9 to preserve absolute destination {:#x}",
                static_cast<std::uintptr_t>(targetAddress),
                static_cast<std::uintptr_t>(absoluteDest));
        } else if (prologueSize >= 5 && gateway[0] == std::byte{ 0xE8 }) {
            REX::WARN(
                "CreateBranchGateway5: captured CALL rel32 at target {:#x} cannot be safely relocated; another mod hooked this function with a CALL trampoline",
                static_cast<std::uintptr_t>(targetAddress));
            return nullptr;
        }

        const REL::ASM::JMP14 jumpBack{ targetAddress + prologueSize };
        std::memcpy(gateway + prologueSize, &jumpBack, sizeof(jumpBack));

        trampoline.write_jmp5(targetAddress, reinterpret_cast<std::uintptr_t>(hook));
        return reinterpret_cast<T>(gateway);
    }

    // Same 5-byte target patch as CreateBranchGateway5, but the copied
    // prologue is rewritten for the small RIP-relative patterns used by
    // PhaseTelemetry's tier-0 DrawWorld hooks.
    inline constexpr std::size_t kMaxRelocatedPrologueBytes = 32;

    inline std::size_t EmitRelocatedPrologue(std::uint8_t* gateway,
                                             std::uintptr_t targetAddress,
                                             const std::uint8_t* original,
                                             std::size_t prologueSize)
    {
        std::size_t s = 0;
        std::size_t d = 0;

        while (s < prologueSize) {
            const std::uint8_t b0 = original[s];

            if (b0 == 0x48 && s + 3 < prologueSize) {
                const std::uint8_t op = original[s + 1];
                const std::uint8_t modrm = original[s + 2];
                if (op == 0x83 && (modrm & 0xC0) == 0xC0) {
                    std::memcpy(gateway + d, original + s, 4);
                    s += 4;
                    d += 4;
                    continue;
                }
                if (op == 0x8B && (modrm & 0xC7) == 0x05 && s + 7 <= prologueSize) {
                    std::int32_t disp32 = 0;
                    std::memcpy(&disp32, original + s + 3, sizeof(disp32));
                    const std::uintptr_t absoluteAddress =
                        targetAddress + s + 7 + static_cast<std::intptr_t>(disp32);
                    const std::uint8_t reg = (modrm >> 3) & 0x07;
                    if (d + 13 > kMaxRelocatedPrologueBytes) {
                        return 0;
                    }
                    gateway[d + 0] = 0x48;
                    gateway[d + 1] = static_cast<std::uint8_t>(0xB8 + reg);
                    std::memcpy(gateway + d + 2, &absoluteAddress, sizeof(absoluteAddress));
                    gateway[d + 10] = 0x48;
                    gateway[d + 11] = 0x8B;
                    gateway[d + 12] = static_cast<std::uint8_t>((reg << 3) | reg);
                    s += 7;
                    d += 13;
                    continue;
                }
            }

            if (b0 == 0x80 && s + 1 < prologueSize) {
                const std::uint8_t modrm = original[s + 1];
                if ((modrm & 0xC7) == 0x05 && s + 7 <= prologueSize) {
                    std::int32_t disp32 = 0;
                    std::memcpy(&disp32, original + s + 2, sizeof(disp32));
                    const std::uint8_t imm8 = original[s + 6];
                    const std::uintptr_t absoluteAddress =
                        targetAddress + s + 7 + static_cast<std::intptr_t>(disp32);
                    const std::uint8_t opExt = (modrm >> 3) & 0x07;
                    if (d + 13 > kMaxRelocatedPrologueBytes) {
                        return 0;
                    }
                    gateway[d + 0] = 0x48;
                    gateway[d + 1] = 0xB8;
                    std::memcpy(gateway + d + 2, &absoluteAddress, sizeof(absoluteAddress));
                    gateway[d + 10] = 0x80;
                    gateway[d + 11] = static_cast<std::uint8_t>(opExt << 3);
                    gateway[d + 12] = imm8;
                    s += 7;
                    d += 13;
                    continue;
                }
            }

            REX::WARN("EmitRelocatedPrologue: reject @ off {} of {} (b0={:02x} next1={:02x} next2={:02x})",
                      s, prologueSize, b0,
                      s + 1 < prologueSize ? original[s + 1] : 0,
                      s + 2 < prologueSize ? original[s + 2] : 0);
            return 0;
        }

        return d;
    }

    template <class T>
    T CreateBranchGateway5Relocated(REL::Relocation<std::uintptr_t>& target, std::size_t prologueSize, void* hook)
    {
        const auto targetAddress = target.address();
        auto& trampoline = REL::GetTrampoline();
        auto* gateway = static_cast<std::byte*>(
            trampoline.allocate(kMaxRelocatedPrologueBytes + sizeof(REL::ASM::JMP14)));
        auto* gatewayBytes = reinterpret_cast<std::uint8_t*>(gateway);

        const auto* original = reinterpret_cast<const std::uint8_t*>(targetAddress);
        const std::size_t written = EmitRelocatedPrologue(gatewayBytes, targetAddress, original, prologueSize);
        if (written == 0) {
            return nullptr;
        }

        const REL::ASM::JMP14 jumpBack{ targetAddress + prologueSize };
        std::memcpy(gatewayBytes + written, &jumpBack, sizeof(jumpBack));

        trampoline.write_jmp5(targetAddress, reinterpret_cast<std::uintptr_t>(hook));
        return reinterpret_cast<T>(gateway);
    }
}
