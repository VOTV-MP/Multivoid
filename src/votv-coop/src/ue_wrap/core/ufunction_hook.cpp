// ue_wrap/core/ufunction_hook.cpp -- see ue_wrap/core/ufunction_hook.h.

#include "ue_wrap/core/ufunction_hook.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <utility>

namespace ue_wrap::ufunction_hook {
namespace {

namespace P = ue_wrap::profile;

// The native exec-thunk ABI (UE4): (Context->*Func)(Stack, Result), so RCX is the context,
// RDX the FFrame and R8 the result, as UFunction::Invoke calls it. Func returns void -- the
// out value flows through *Result -- so a void thunk preserves the contract.
using NativeFuncPtr = void(__fastcall*)(void* context, void* stack, void* result);

struct Slot {
    void*              ufunction = nullptr;
    NativeFuncPtr      original  = nullptr;
    PostNativeCallback cb        = nullptr;
};

// This facility is the STANDARD seam for every dispatch our ProcessEvent detour cannot see
// (the EX_* inner calls, the post-BUA AnimBP overrides), so its user count grows with the mod.
// Each slot owns a distinct STAMPED thunk that closes over its slot index as a compile-time
// constant: no per-call table lookup, and no dependence on FFrame's current-native-function
// field. Installs happen on the game thread and native dispatch is on the game thread, so
// there is no cross-thread race.
//
// Capacity is a compile-time bound, since stamped thunks need one, and the thunk table below
// is GENERATED from this constant, so growing it means editing this one line. Size it for the
// whole roster rather than a handful: peers install asymmetrically, so a table that fits one
// role can be full on the other, and a hook that cannot install makes a fix work on one peer
// and not the other. The count here covers the standing installs plus the driver natives and
// QuitGame that a probe patches on top.
constexpr int kMaxNativeHooks = 40;
Slot g_slots[kMaxNativeHooks];
int  g_slotCount = 0;

// SEH-only callback dispatch (no C++ destructors in this frame -- MSVC forbids mixing
// __try/__except with C++ unwind; same contract as game_thread::RunObserverSEH). We are
// DEEP inside the engine's spawn call, so a fault in the gameplay cb must be absorbed,
// not crash the engine. Returns 0 clean, 1 if a fault was caught.
int RunCbSEH(PostNativeCallback cb, void* context, void* src, void* result) {
    __try {
        cb(context, src, result);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

// Re-entrancy guard. A callback must not spawn an actor synchronously -- none does; the host
// convert only re-binds the element and queues a reliable -- but if one ever did, its deferred
// spawn would re-enter this thunk, so the callback is skipped on re-entry and a nested spawn
// can never double-fire. The forward ALWAYS runs: a re-entrant spawn must still proceed.
// Thread-local, though native dispatch is game-thread anyway.
//
// The guard is GLOBAL across ALL slots, not per-slot, so a hooked native dispatched from
// inside another slot's callback would have its own callback skipped too. That is safe only
// while no callback dispatches a hooked native synchronously -- sequential Blueprint bytecode
// steps are not nested, since the first callback returns and clears the flag before the next
// step runs. Preserve that when adding a callback, or make the guard per-slot.
thread_local bool t_inCb = false;

template <int N>
void __fastcall NativeThunk(void* context, void* stack, void* result) {
    Slot& s = g_slots[N];
    // FFrame::Object is the actor whose bytecode is executing -- the SOURCE entity for a spawn
    // issued from its ubergraph. Read BEFORE forwarding: the original steps parameters off the
    // bytecode stream, but never touches Object.
    void* srcObj = stack
        ? *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(stack) + P::off::FFrame_Object)
        : nullptr;
    // Transparent forward: steps the params, runs the impl, writes *result.
    s.original(context, stack, result);
    if (!s.cb || t_inCb) return;   // re-entrant cb -> skip (no double-convert from a nested spawn)
    // *Result = the native fn's RESULT_PARAM (the spawned actor for BeginDeferred). NULL-safe:
    // a failed spawn leaves it null -> the cb gets null + logs it (never derefs blind).
    void* spawned = result ? *reinterpret_cast<void**>(result) : nullptr;
    t_inCb = true;
    const int rc = RunCbSEH(s.cb, context, srcObj, spawned);
    t_inCb = false;
    if (rc != 0) {
        UE_LOGE("ufunction_hook: post-native cb AV absorbed (slot %d, ufn=%p src=%p result=%p) -- "
                "engine continues", N, s.ufunction, srcObj, spawned);
    }
}

// One distinct stamped thunk per slot, generated FROM kMaxNativeHooks -- the table can
// never under-enumerate the capacity (the old hand-written switch could, and its
// static_assert pinned the constant instead of following it).
template <size_t... Is>
constexpr std::array<NativeFuncPtr, sizeof...(Is)> MakeThunkTable(std::index_sequence<Is...>) {
    return {{&NativeThunk<static_cast<int>(Is)>...}};
}
constexpr std::array<NativeFuncPtr, kMaxNativeHooks> g_thunkTable =
    MakeThunkTable(std::make_index_sequence<kMaxNativeHooks>{});

NativeFuncPtr ThunkFor(int n) {
    return (n >= 0 && n < kMaxNativeHooks) ? g_thunkTable[static_cast<size_t>(n)] : nullptr;
}

}  // namespace

bool InstallPostHook(void* ufunction, PostNativeCallback cb) {
    if (!ufunction || !cb) return false;
    // Idempotent: the same (ufunction, cb) re-install is a no-op (the caller's Install
    // retries each world-gated pass until the class resolves).
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].ufunction == ufunction && g_slots[i].cb == cb) return true;
    }
    if (g_slotCount >= kMaxNativeHooks) {
        UE_LOGE("ufunction_hook: table full (%d slots) -- cannot patch ufn=%p (grow kMaxNativeHooks; "
                "the thunk table is generated from it)", kMaxNativeHooks, ufunction);
        return false;
    }
    auto* funcSlot = reinterpret_cast<NativeFuncPtr*>(
        reinterpret_cast<uint8_t*>(ufunction) + P::off::UFunction_Func);
    NativeFuncPtr original = *funcSlot;
    if (!original) {
        // Func is the native exec thunk (set at StaticRegisterNatives) -- never null for a
        // native UFunction. Null here = the offset is wrong for this build -> REFUSE (a bad
        // write would corrupt an unrelated UFunction field).
        UE_LOGE("ufunction_hook: ufn=%p Func @0x%zX reads null -- offset wrong for this build? NOT patching",
                ufunction, static_cast<size_t>(P::off::UFunction_Func));
        return false;
    }
    const int n = g_slotCount;
    g_slots[n].ufunction = ufunction;
    g_slots[n].original  = original;
    g_slots[n].cb        = cb;
    g_slotCount = n + 1;     // slot fully populated before the thunk can be reached
    // An 8-byte aligned pointer swap: the Func slot is 8-aligned and the UFunction lives in the
    // writable UE4 object pool. Atomic on x64, and game-thread-only dispatch means no torn read
    // in any case.
    *funcSlot = ThunkFor(n);
    UE_LOGI("ufunction_hook: patched ufn=%p Func @0x%zX (orig=%p -> thunk slot %d) -- standalone "
            "UFunction::Func hook (catches EX_CallMath calls invisible to ProcessEvent)",
            ufunction, static_cast<size_t>(P::off::UFunction_Func), original, n);
    return true;
}

}  // namespace ue_wrap::ufunction_hook
