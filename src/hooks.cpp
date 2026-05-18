#include <PCH.h>
#include <hooks.h>

namespace Hooks
{
    namespace Addresses
    {
        REL::Relocation<std::uintptr_t> D3D11CreateDeviceAndSwapChainCall{ REL::ID{ 224250, 4492363 } };
        REL::Relocation<std::uintptr_t> ClipCursor{ REL::ID{ 641385, 4823626 } };

        REL::Relocation<std::uintptr_t> BSBatchRendererDraw{ REL::ID{ 1152191, 2318696 } };
        REL::Relocation<std::uintptr_t> BipedAnimAttachSkinnedObject{ REL::ID{ 1575810, 2194388 } };
        REL::Relocation<std::uintptr_t> BipedAnimAttachBipedWeapon{ REL::ID{ 788361, 2194353 } };
        REL::Relocation<std::uintptr_t> BipedAnimAttachToParent{ REL::ID{ 1370428, 2194378 } };
        REL::Relocation<std::uintptr_t> BipedAnimRemovePart{ REL::ID{ 575576, 2194342 } };
        REL::Relocation<std::uintptr_t> Update3DModel{ REL::ID{ 986782, 2231882 } };
        REL::Relocation<std::uintptr_t> Reset3D{ REL::ID{ 302888, 2229913 } };
        REL::Relocation<std::uintptr_t> RenderBatches{ REL::ID{ 1083446, 0 } };
        REL::Relocation<std::uintptr_t> RenderGeometryGroup{ REL::ID{ 1379976, 2317897 } };
        REL::Relocation<std::uintptr_t> FinishAccumulatingShadowMapOrMask{ REL::ID{ 1358523, 2317874 } };
        REL::Relocation<std::uintptr_t> RenderCommandBufferPassesImpl{ REL::ID{ 1184461, 2318711 } };
        REL::Relocation<std::uintptr_t> RenderPersistentPassListImpl{ REL::ID{ 1170655, 2318712 } };
        REL::Relocation<std::uintptr_t> ProcessCommandBuffer{ REL::ID{ 673619, 0 } }; // OG 1.10.163: 0x141D13A10
        REL::Relocation<std::uintptr_t> RenderPassImpl{ REL::ID{ 1543785, 2318710 } };
        REL::Relocation<std::uintptr_t> RegisterObjectShadowMapOrMask{ REL::ID{ 1071289, 2317861 } };
        REL::Relocation<std::uintptr_t> RegisterPassGeometryGroup{ REL::ID{ 197098, 0 } };
        REL::Relocation<std::uintptr_t> BuildCommandBuffer{ REL::ID{ 833764, 2318870 } };
        REL::Relocation<std::uintptr_t> BSLightTestFrustumCull{ REL::ID{ 1440624, 0 } };
        REL::Relocation<std::uintptr_t> TryAddTiledLightLambda{ REL::ID{ 999390, 0 } };
        REL::Relocation<std::uintptr_t> SetupPointLightGeometry{ REL::ID{ 212931, 0 } };
    }

    bool PatchVTableSlot(void** slot, void* hook)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }

        *slot = hook;

        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
        return true;
    }
}
