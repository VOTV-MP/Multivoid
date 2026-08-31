// ue_wrap/engine/level_travel.cpp -- the OpenLevel detour. See the header for the seam's
// rationale and for the veto's DATA-ONLY contract.

#include "ue_wrap/engine/level_travel.h"

#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sig_scan.h"

#include <windows.h>

#include <atomic>

namespace ue_wrap::engine::level_travel {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// MinHook's TRAMPOLINE SLOT, not the target -- see hook.h's naming note. Never removed
// (the facade offers no remove at all), so it stays valid for the life of the process.
using OpenLevelFn = void(__fastcall*)(void*, uint64_t, bool, void*);
OpenLevelFn g_openLevelTrampoline = nullptr;

std::atomic<VetoFn> g_veto{nullptr};
std::atomic<bool>   g_installed{false};
std::atomic<bool>   g_attempted{false};
std::atomic<uint64_t> g_vetoCount{0};
std::atomic<uint64_t> g_seenCount{0};

// The by-value `FString Options` parameter, as MSVC passes it: a pointer to a
// {TCHAR* Data; int32 ArrayNum; int32 ArrayMax} the CALLER built and the CALLEE destroys.
// `[V]` the decompile frees `*a4` on every return path, and the exec thunk frees its OWN
// stepped buffer (a different allocation) after the call returns. So the ownership rule is
// exact and asymmetric:
//   * call the trampoline -> the engine frees it. We must NOT.
//   * refuse the travel    -> nobody frees it. We must, or it leaks.
// On the death path Options is the empty literal, so Data is null and this does nothing;
// the code exists for every OTHER caller a future veto might refuse.
void FreeByValueFString(void* options) {
    if (!options) return;
    auto** data = reinterpret_cast<void**>(options);
    if (*data) {
        R::EngineFree(*data);
        *data = nullptr;
    }
    auto* counts = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(options) + 8);
    counts[0] = 0;  // ArrayNum
    counts[1] = 0;  // ArrayMax
}

// SEH-only frame (no C++ objects needing unwind) so the __try is legal. This is the crash
// firewall around a callback we do not own: a faulting veto must not take the game's level
// travel down with it. FAIL OPEN on a fault -- let the travel proceed. That direction is
// deliberate and matches DEATH_ARC section 3.1: a player who reaches the main menu is
// recoverable, a player stranded in a world we refused to leave for a reason we could not
// evaluate is not.
bool AskVeto(VetoFn fn, void* worldContextObject, uint64_t levelName, bool bAbsolute) {
    __try {
        return fn(worldContextObject, levelName, bAbsolute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall OpenLevelDetour(void* worldContextObject, uint64_t levelName,
                                bool bAbsolute, void* options) {
    g_seenCount.fetch_add(1, std::memory_order_relaxed);
    VetoFn fn = g_veto.load(std::memory_order_acquire);
    if (fn && AskVeto(fn, worldContextObject, levelName, bAbsolute)) {
        g_vetoCount.fetch_add(1, std::memory_order_relaxed);
        FreeByValueFString(options);
        return;  // the whole cancel: SetClientTravel never runs, so no travel is requested
    }
    if (g_openLevelTrampoline)
        g_openLevelTrampoline(worldContextObject, levelName, bAbsolute, options);
}

}  // namespace

bool Install() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    if (g_attempted.exchange(true, std::memory_order_acq_rel)) return false;  // failed once

    const uintptr_t addr = ue_wrap::FindPattern(P::kSigOpenLevel);
    if (!addr) {
        // The signature is in the .exe image, so a miss is a STALE SIGNATURE for this game
        // build, not a timing problem -- retrying cannot help. Log once and give up; the
        // caller's fallback (net_pump's flee) is what keeps a death survivable without us.
        UE_LOGE("level_travel: UGameplayStatics::OpenLevel signature not found -- the travel "
                "seam is NOT installed (sdk_profile.h::kSigOpenLevel stale for this build?). "
                "Nothing can veto a level travel; death falls back to the menu flee.");
        return false;
    }

    ue_wrap::hook::Init();  // idempotent
    if (!ue_wrap::hook::Install(reinterpret_cast<void*>(addr),
                                reinterpret_cast<void*>(&OpenLevelDetour),
                                reinterpret_cast<void**>(&g_openLevelTrampoline))) {
        UE_LOGE("level_travel: MinHook install on OpenLevel@%p FAILED -- travel seam NOT active",
                reinterpret_cast<void*>(addr));
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    UE_LOGI("level_travel: seam INSTALLED (UGameplayStatics::OpenLevel@%p; pass-through until "
            "a veto is published)", reinterpret_cast<void*>(addr));
    return true;
}

bool IsInstalled() { return g_installed.load(std::memory_order_acquire); }

void SetVeto(VetoFn fn) { g_veto.store(fn, std::memory_order_release); }

uint64_t VetoCount() { return g_vetoCount.load(std::memory_order_relaxed); }
uint64_t SeenCount() { return g_seenCount.load(std::memory_order_relaxed); }

}  // namespace ue_wrap::engine::level_travel
