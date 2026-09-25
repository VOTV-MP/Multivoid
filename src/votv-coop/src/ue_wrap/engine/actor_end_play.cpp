// ue_wrap/engine/actor_end_play.cpp -- see actor_end_play.h.

#include "ue_wrap/engine/actor_end_play.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sig_scan.h"

#include <windows.h>  // SEH (__try/__except) -- the firewall around the sinks

#include <atomic>

namespace ue_wrap::actor_end_play {
namespace {

namespace GT = ue_wrap::game_thread;
namespace prof = ue_wrap::profile;

// void AActor::EndPlay(const EEndPlayReason::Type Reason): this in RCX, the reason in EDX.
using EndPlayFn = void(__fastcall*)(void* actor, uint32_t reason);
EndPlayFn g_trampoline = nullptr;

std::atomic<bool> g_installed{false};

// The sinks: a slot is written once with release and read with acquire.
std::atomic<Sink> g_sinks[kMaxSinks] = {};
std::atomic<int> g_sinkCount{0};

std::atomic<unsigned long long> g_seen{0};
std::atomic<unsigned long long> g_offGameThread{0};

// The engine's own test, the one the signature ends on: EndPlay does its work only for an actor
// whose begun-play state reads HasBegunPlay.
bool HasBegunPlay(const void* actor) {
    const uint8_t state = *(static_cast<const uint8_t*>(actor) + prof::kActor_BegunPlayByte);
    return (state & prof::kActor_BegunPlayMask) == prof::kActor_HasBegunPlay;
}

// A sink that faults loses this end of play, never the engine's: the fault is absorbed here, inside
// an SEH-only frame (no C++ objects to unwind, so the __try is legal), and said the first few times.
void RunSinksSEH(void* actor, Reason reason) {
    __try {
        const int n = g_sinkCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (Sink s = g_sinks[i].load(std::memory_order_acquire)) s(actor, reason);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static std::atomic<int> s_said{0};
        if (s_said.fetch_add(1, std::memory_order_relaxed) < 5)
            UE_LOGE("actor_end_play: a sink FAULTED (exception 0x%08lX) -- the sinks after it missed this "
                    "end of play", GetExceptionCode());
    }
}

void __fastcall EndPlayDetour(void* actor, uint32_t reason) {
    if (actor && HasBegunPlay(actor)) {
        g_seen.fetch_add(1, std::memory_order_relaxed);
        if (GT::IsDefinitelyOffGameThread())
            g_offGameThread.fetch_add(1, std::memory_order_relaxed);
        else
            RunSinksSEH(actor, static_cast<Reason>(reason));
    }
    g_trampoline(actor, reason);
}

}  // namespace

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    const uintptr_t addr = ue_wrap::FindPattern(prof::kSigActorEndPlay);
    if (!addr) {
        UE_LOGE("actor_end_play: AActor::EndPlay's signature did not match this build (sdk_profile.h "
                "kSigActorEndPlay) -- no actor's end of play is seen");
        return false;
    }
    ue_wrap::hook::Init();  // idempotent
    // UE4SS detours the same function (its HookEndPlay, on by default) and follows a jmp it finds there:
    // it resolved EndPlay into our relay and patched that, and the host hung at boot. The immune relay
    // is the one ProcessEvent and the script loop install through, so both detours compose.
    if (!ue_wrap::hook::Install(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&EndPlayDetour),
                                reinterpret_cast<void**>(&g_trampoline), /*followJmpImmune=*/true)) {
        UE_LOGE("actor_end_play: the detour on AActor::EndPlay@%p did not install -- no actor's end of play "
                "is seen", reinterpret_cast<void*>(addr));
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("actor_end_play: AActor::EndPlay detoured (%p) -- every actor that began play is seen ending it",
            reinterpret_cast<void*>(addr));
    return true;
}

bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }

bool AddSink(Sink sink) {
    if (!sink) return false;
    const int n = g_sinkCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_sinks[i].load(std::memory_order_acquire) == sink) return true;
    if (n >= kMaxSinks) {
        UE_LOGE("actor_end_play: the sink table is full (%d) -- a sink was refused", kMaxSinks);
        return false;
    }
    g_sinks[n].store(sink, std::memory_order_release);
    g_sinkCount.store(n + 1, std::memory_order_release);
    return true;
}

Stats GetStats() {
    return {g_seen.load(std::memory_order_relaxed), g_offGameThread.load(std::memory_order_relaxed)};
}

}  // namespace ue_wrap::actor_end_play
