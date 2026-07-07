// ue_wrap/ufunction_hook.cpp -- see ue_wrap/ufunction_hook.h.

#include "ue_wrap/ufunction_hook.h"

#include "ue_wrap/log.h"
#include "ue_wrap/sdk_profile.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <utility>

namespace ue_wrap::ufunction_hook {
namespace {

namespace P = ue_wrap::profile;

// The native exec-thunk ABI (UE4): (Context->*Func)(Stack, Result) -- RCX=Context,
// RDX=FFrame*, R8=Result. IDA-confirmed in UFunction::Invoke (0x141302DC0):
// `(*(Function+0xD8))(Object, &Frame, Result)`. Func is void-returning (the out-value
// flows through *Result, not the return), so a void thunk preserves the contract.
using NativeFuncPtr = void(__fastcall*)(void* context, void* stack, void* result);

struct Slot {
    void*              ufunction = nullptr;
    NativeFuncPtr      original  = nullptr;
    PostNativeCallback cb        = nullptr;
};

// This facility is the STANDARD seam for every dispatch our ProcessEvent detour cannot
// see (EX_* inner calls, post-BUA AnimBP overrides -- docs/COOP_DISPATCH_VISIBILITY.md),
// so its user count GROWS with the mod. Each slot owns a distinct STAMPED thunk
// (NativeThunk<N>) so it closes over its slot index as a compile-time constant -- no
// per-call table lookup, no dependence on FFrame::CurrentNativeFunction (@+0x88).
// Writes happen on the game thread (install) and the thunks run on the game thread
// (native dispatch is GT) -> no cross-thread race.
//
// Capacity is a compile-time bound (stamped thunks require one); the thunk table below
// is GENERATED from this constant, so growing capacity = editing this ONE line. Born
// 2026-07-02: at 4 slots the puppet head-gate hook was the HOST's 5th install (save-
// indicator x2 + host-only trash-collect x2 filled the table; the client had a free
// slot) -> "table full" -> the head fix silently worked on one peer and not the other.
// Asymmetric peers = asymmetric slot pressure: size for the whole roster, not "a
// handful".
constexpr int kMaxNativeHooks = 16;
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

// Re-entrancy guard: the gameplay cb (a host convert) must NOT spawn an actor synchronously -- it doesn't
// (OnHostConvert only re-binds the Element + queues a reliable packet) -- but if a future cb ever did, its
// BeginDeferred would re-enter THIS thunk. Skip the cb on re-entry so a nested spawn can never double-fire
// the convert. Thread-local (native dispatch is game-thread, but the guard stays correct regardless). The
// forward ALWAYS runs (a re-entrant spawn must still proceed).
// NOTE (audit 2026-07-07): this guard is GLOBAL across ALL slots, not per-slot -- a hooked native
// dispatched from INSIDE another slot's cb would have its cb silently skipped too. Safe today because
// no cb dispatches a hooked native synchronously (sequential BP bytecode steps -- e.g. a pile's toClump
// BeginDeferred cb returns, t_inCb resets, THEN the pile's K2_DestroyActor fires -- are NOT nested);
// preserve that property when adding cbs, or make the guard per-slot.
thread_local bool t_inCb = false;

template <int N>
void __fastcall NativeThunk(void* context, void* stack, void* result) {
    Slot& s = g_slots[N];
    // FFrame::Object (@0x18) = the actor whose bytecode is executing = the SOURCE entity
    // for a spawn issued from its ubergraph (the re-piling clump). Read BEFORE forwarding
    // (the original steps params off the bytecode stream but never touches Object).
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
    // 8-byte aligned pointer swap (UFunction+0xD8 is 8-aligned; the UFunction lives in the
    // writable UE4 object pool). Atomic on x64; GT-only dispatch means no torn read anyway.
    *funcSlot = ThunkFor(n);
    UE_LOGI("ufunction_hook: patched ufn=%p Func @0x%zX (orig=%p -> thunk slot %d) -- standalone "
            "UFunction::Func hook (catches EX_CallMath calls invisible to ProcessEvent)",
            ufunction, static_cast<size_t>(P::off::UFunction_Func), original, n);
    return true;
}

}  // namespace ue_wrap::ufunction_hook
