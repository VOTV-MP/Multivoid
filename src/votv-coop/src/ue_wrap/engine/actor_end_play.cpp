// ue_wrap/engine/actor_end_play.cpp -- see actor_end_play.h.

#include "ue_wrap/engine/actor_end_play.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
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

// The engine's own test, the one Install checks the body for: EndPlay does its work only for an actor
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
    // Where the engine finds it: RouteEndPlay calls EndPlay through the vtable, and Actor's own, read
    // from its class default object, names the function every actor reaches through Super. Its first
    // bytes are no guide: a detour installed before ours owns them.
    void* const cdo = ue_wrap::reflection::FindClassDefaultObject(L"Actor");
    if (!cdo) {
        UE_LOGE("actor_end_play: Default__Actor is not in the object array -- no actor's end of play is seen");
        return false;
    }
    uintptr_t image = 0;
    size_t imageSize = 0;
    ue_wrap::MainModuleRange(image, imageSize);
    const auto inImage = [&](uintptr_t p) { return p >= image && p < image + imageSize; };
    const auto* const vtbl = *static_cast<const uintptr_t* const*>(cdo);
    const uintptr_t addr = inImage(reinterpret_cast<uintptr_t>(vtbl))
                               ? vtbl[prof::kActor_EndPlay_VtblOff / sizeof(uintptr_t)] : 0;
    if (!inImage(addr) || !ue_wrap::MatchesAt(addr + prof::kActorEndPlayBodyOff, prof::kActorEndPlayBody)) {
        UE_LOGE("actor_end_play: Actor's vtable at +0x%zX (%p) does not lead to AActor::EndPlay's begun-play "
                "test on this build (sdk_profile.h kActor_EndPlay_VtblOff, kActorEndPlayBody) -- no actor's "
                "end of play is seen", prof::kActor_EndPlay_VtblOff, reinterpret_cast<void*>(addr));
        return false;
    }
    // Whose bytes the entry held: the engine's prologue, a jump (rel32, rel8 or through a pointer, the
    // forms a detour writes), or something else.
    const auto* entry = reinterpret_cast<const uint8_t*>(addr);
    const char* const held = ue_wrap::MatchesAt(addr, prof::kActorEndPlayPrologue) ? "the engine's prologue"
                             : (entry[0] == 0xE9 || entry[0] == 0xEB || (entry[0] == 0xFF && entry[1] == 0x25))
                                 ? "another detour's jump, which now runs after ours"
                                 : "neither the engine's prologue nor a jump";
    ue_wrap::hook::Init();  // idempotent
    // UE4SS detours the same function (its HookEndPlay, on by default), after ours or before it. After,
    // it follows the jmp it finds there: it resolved EndPlay into our relay and patched that, and the
    // host hung at boot; the immune relay, the one ProcessEvent and the script loop install through,
    // lets both detours compose. Before, MinHook carries its jump into our trampoline: with the boot's
    // VOTVCOOP_PE_INSTALL_DELAY_MS holding our installs back, both peers' entries held UE4SS's jump,
    // and the sinks still heard both peers' destroys in a drill.
    if (!ue_wrap::hook::Install(reinterpret_cast<void*>(addr), reinterpret_cast<void*>(&EndPlayDetour),
                                reinterpret_cast<void**>(&g_trampoline), /*followJmpImmune=*/true)) {
        UE_LOGE("actor_end_play: the detour on AActor::EndPlay@%p did not install -- no actor's end of play "
                "is seen", reinterpret_cast<void*>(addr));
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("actor_end_play: AActor::EndPlay detoured (%p, exe+0x%zX; its entry held %s) -- every actor that "
            "began play is seen ending it", reinterpret_cast<void*>(addr), static_cast<size_t>(addr - image), held);
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
