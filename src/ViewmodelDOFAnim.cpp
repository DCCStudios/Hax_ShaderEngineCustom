#include <PCH.h>   // RE/Fallout.h, F4SE, REL
#include "ViewmodelDOFAnim.h"

#include <Global.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_map>

namespace
{
    // Suppression state, set by the main-thread animation hooks, read by the
    // render-thread smoother.
    std::atomic<bool> s_reloadActive{ false };
    std::atomic<bool> s_idleActive{ false };

    // Patch a single 8-byte pointer (a vtable slot) and return the previous value
    // as the same pointer type. Mirrors SeamlessInspect's SafeWrite64Function.
    template <typename Ty>
    Ty SafeWrite64Function(std::uintptr_t a_addr, Ty a_data)
    {
        DWORD oldProtect;
        void* raw[2];
        std::memcpy(raw, &a_data, sizeof(a_data));
        const std::size_t len = sizeof(raw[0]);

        ::VirtualProtect(reinterpret_cast<void*>(a_addr), len, PAGE_EXECUTE_READWRITE, &oldProtect);
        Ty old{};
        std::memcpy(&old, reinterpret_cast<void*>(a_addr), len);
        std::memcpy(reinterpret_cast<void*>(a_addr), &raw[0], len);
        ::VirtualProtect(reinterpret_cast<void*>(a_addr), len, oldProtect, &oldProtect);
        return old;
    }

    // ---- Player animation-graph event sink hook ---------------------------------
    // player + 0x38 is a BSTEventSink<BSAnimationGraphEvent>; its ProcessEvent is
    // vtable slot +0x8 (SeamlessInspect's proven offsets). The vtable is shared, so
    // the hook fires for every actor's sink - we filter to the player's own sink
    // instance so an NPC reloading can't suppress the player's viewmodel DOF.

    // The player's sink subobject (player + 0x38), captured at install time.
    void* s_playerSink = nullptr;

    class AnimGraphEventWatcher
    {
    public:
        using FnProcessEvent = RE::BSEventNotifyControl (AnimGraphEventWatcher::*)(
            RE::BSAnimationGraphEvent& a_event,
            RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source);

        RE::BSEventNotifyControl HookedProcessEvent(
            RE::BSAnimationGraphEvent& a_event,
            RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_source)
        {
            // Only the player's own graph drives suppression.
            if (this == s_playerSink) {
                const RE::BSFixedString& tag = a_event.tag;
                if (tag == "reloadStart") {
                    s_reloadActive.store(true, std::memory_order_relaxed);
                } else if (tag == "reloadComplete" || tag == "ReloadComplete" ||
                           tag == "ReloadEnd" || tag == "reloadEnd") {
                    s_reloadActive.store(false, std::memory_order_relaxed);
                } else if (tag == "IdleStop") {
                    s_idleActive.store(false, std::memory_order_relaxed);
                }
            }

            FnProcessEvent original = s_fnHash.at(*reinterpret_cast<std::uintptr_t*>(this));
            return original ? (this->*original)(a_event, a_source)
                            : RE::BSEventNotifyControl::kContinue;
        }

        void HookSink()
        {
            const std::uintptr_t vtable = *reinterpret_cast<std::uintptr_t*>(this);
            if (s_fnHash.find(vtable) == s_fnHash.end()) {
                FnProcessEvent original = SafeWrite64Function(
                    vtable + 0x8, &AnimGraphEventWatcher::HookedProcessEvent);
                s_fnHash.emplace(vtable, original);
            }
        }

        static std::unordered_map<std::uintptr_t, FnProcessEvent> s_fnHash;
    };
    std::unordered_map<std::uintptr_t, AnimGraphEventWatcher::FnProcessEvent>
        AnimGraphEventWatcher::s_fnHash;

    // ---- SetupSpecialIdle hook (idle start) -------------------------------------
    // The game routes special idles (inspect / examine / weapon check) through
    // AIProcess::SetupSpecialIdle. Catching it flags "idle active"; the IdleStop
    // event above clears it. REL::IDs are SeamlessInspect's, valid on 1.10.163.
    using FnSetupSpecialIdle = bool (*)(RE::AIProcess*, RE::Actor&, RE::DEFAULT_OBJECT,
                                        RE::TESIdleForm*, bool, RE::TESObjectREFR*);
    std::uintptr_t s_setupSpecialIdleOrig = 0;

    bool HookedSetupSpecialIdle(RE::AIProcess* a_ai, RE::Actor& a_actor,
                                RE::DEFAULT_OBJECT a_obj, RE::TESIdleForm* a_idle,
                                bool a_arg5, RE::TESObjectREFR* a_target)
    {
        if (&a_actor == RE::PlayerCharacter::GetSingleton()) {
            s_idleActive.store(true, std::memory_order_relaxed);
        }
        auto original = reinterpret_cast<FnSetupSpecialIdle>(s_setupSpecialIdleOrig);
        return original ? original(a_ai, a_actor, a_obj, a_idle, a_arg5, a_target) : false;
    }

    bool s_installed = false;
}

namespace ViewmodelDOFAnim
{
    void Install()
    {
        if (s_installed) {
            return;
        }
        s_installed = true;

        // Player animation-graph event sink (reload events + IdleStop).
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            s_playerSink = reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(player) + 0x38);
            reinterpret_cast<AnimGraphEventWatcher*>(s_playerSink)->HookSink();
        }

        // SetupSpecialIdle (idle start). Two call sites, both redirected through
        // the global trampoline (matching the codebase's write_call convention).
        REL::Relocation<std::uintptr_t> call1{ REL::ID(1379254), 0xDA };
        REL::Relocation<std::uintptr_t> call2{ REL::ID(760592), 0x3C };
        s_setupSpecialIdleOrig = call1.write_call<5>(
            reinterpret_cast<std::uintptr_t>(&HookedSetupSpecialIdle));
        call2.write_call<5>(
            reinterpret_cast<std::uintptr_t>(&HookedSetupSpecialIdle));
    }

    float Update(float a_deltaSeconds, float a_blendTimeSeconds)
    {
        static float s_blend = 1.0f;
        const bool suppress = s_reloadActive.load(std::memory_order_relaxed) ||
                              s_idleActive.load(std::memory_order_relaxed);
        const float target = suppress ? 0.0f : 1.0f;
        const float rate = a_blendTimeSeconds > 1.0e-4f
            ? (std::min)(1.0f, a_deltaSeconds / a_blendTimeSeconds)
            : 1.0f;
        s_blend += (target - s_blend) * rate;
        s_blend = std::clamp(s_blend, 0.0f, 1.0f);
        return s_blend;
    }
}
