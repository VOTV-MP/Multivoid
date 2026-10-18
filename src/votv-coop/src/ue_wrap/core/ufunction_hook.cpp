// ue_wrap/core/ufunction_hook.cpp -- see ue_wrap/core/ufunction_hook.h.

#include "ue_wrap/core/ufunction_hook.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <utility>

namespace ue_wrap::ufunction_hook {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
using script_gate::Verdict;

// The native exec-thunk ABI (UE4): (Context->*Func)(Stack, Result), so RCX is the context,
// RDX the FFrame and R8 the result, as UFunction::Invoke calls it. Func returns void -- the
// out value flows through *Result -- so a void thunk preserves the contract.
using NativeFuncPtr = void(__fastcall*)(void* context, void* stack, void* result);

// A pre hook's frame: its parameters are stepped into this many bytes on the thunk's own stack, and
// a function whose parameters reach past it (less the slack a bool's four-byte VM write needs) is
// refused at install. The VM ends a call's parameters with this opcode (P_FINISH steps over it).
constexpr int     kMaxPreParams = 16;
constexpr int     kPreFrameBytes = 256;
constexpr int     kPreFrameSlack = 8;
constexpr uint8_t kExEndFunctionParms = 0x16;
// A const reference is an out reference marked const; a pre hook steps and reads it as an input.
constexpr uint64_t kConstRef = P::cpf::OutParm | P::cpf::ReferenceParm | P::cpf::ConstParm;

struct Slot {
    void*              ufunction = nullptr;
    NativeFuncPtr      original  = nullptr;
    PostNativeCallback cb        = nullptr;
    PreNativeCallback  pre       = nullptr;   // set for a pre hook, whose thunk is PreThunk
    std::atomic<bool>  armed{false};
    // A pre hook's parameters in declaration order, each one's offset in the frame.
    int     paramCount = 0;
    int16_t paramOffsets[kMaxPreParams] = {};
    bool    streamMismatchLogged = false;
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
// and not the other. The count here covers the standing installs, a dev build's probes (the driver
// natives and QuitGame among them) and the dev selftest's hooks, with headroom.
constexpr int kMaxNativeHooks = 56;
Slot g_slots[kMaxNativeHooks];
int  g_slotCount = 0;
// The VM's exec-handler table, taken from script_gate by the first pre hook's install.
const script_gate::ExecHandler* g_exec = nullptr;

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

// A stack overflow is passed on, as script_gate's firewall passes it: the guard page is gone, and absorbing it
// would run the engine on an exhausted stack.
int PreFaultFilter(EXCEPTION_POINTERS* ep) {
    return ep->ExceptionRecord->ExceptionCode == static_cast<DWORD>(EXCEPTION_STACK_OVERFLOW)
               ? EXCEPTION_CONTINUE_SEARCH
               : EXCEPTION_EXECUTE_HANDLER;
}

int RunPreCbSEH(PreNativeCallback cb, void* context, void* src, const uint8_t* parms, Verdict* verdict) {
    __try {
        *verdict = cb(context, src, parms);
        return 0;
    } __except (PreFaultFilter(GetExceptionInformation())) {
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

// The frame and the result storage of the call the running callback reports, published for
// exactly as long as the callback runs (CurrentCallerFrame, CurrentResult).
thread_local CallerFrame t_frame{nullptr, nullptr};
thread_local void*       t_result = nullptr;

inline void* ReadPtr(void* base, size_t off) {
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off);
}

// Where the frame's bytecode stands, from the start of the frame's function's Script data; -1 with no code
// pointer (a native reached by ProcessEvent), no function or a pointer outside the function's own script.
int32_t CodeOffsetOf(void* stack) {
    const uint8_t* code = static_cast<const uint8_t*>(ReadPtr(stack, P::off::FFrame_Code));
    void* fn = ReadPtr(stack, P::off::FFrame_Node);
    if (!code || !fn) return -1;
    const uint8_t* script = static_cast<const uint8_t*>(ReadPtr(fn, P::off::UStruct_Script));
    const int32_t num = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(fn) + P::off::UStruct_ScriptNum);
    if (!script || code < script || code > script + num) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGE("ufunction_hook: a frame's code pointer lies outside its function's script (script %p, "
                    "%d bytes, code %p) -- reported as no bytecode caller", static_cast<const void*>(script), num,
                    static_cast<const void*>(code));
        }
        return -1;
    }
    return static_cast<int32_t>(code - script);
}

template <int N>
void __fastcall NativeThunk(void* context, void* stack, void* result) {
    Slot& s = g_slots[N];
    // Transparent forward: steps the params, runs the impl, writes *result.
    s.original(context, stack, result);
    // Disarmed, or a re-entrant cb -> skip (no double-convert from a nested spawn).
    if (!s.armed.load(std::memory_order_relaxed) || !s.cb || t_inCb) return;
    // FFrame::Object is the actor whose bytecode is executing -- the SOURCE entity for a spawn
    // issued from its ubergraph -- and Node and Locals are the function and storage of that
    // frame. Read after the forward: stepping the parameters moves the frame's code pointer and
    // never writes those three. The code pointer stands at the call's end by then.
    void* srcObj = stack ? ReadPtr(stack, P::off::FFrame_Object) : nullptr;
    CallerFrame frame{nullptr, nullptr};
    if (stack) {
        frame.function = ReadPtr(stack, P::off::FFrame_Node);
        frame.locals = static_cast<uint8_t*>(ReadPtr(stack, P::off::FFrame_Locals));
        frame.codeOffset = CodeOffsetOf(stack);
    }
    // *Result = the native fn's RESULT_PARAM (the spawned actor for BeginDeferred). NULL-safe:
    // a failed spawn leaves it null -> the cb gets null + logs it (never derefs blind).
    void* spawned = result ? *reinterpret_cast<void**>(result) : nullptr;
    t_inCb = true;
    t_frame = frame;
    t_result = result;
    const int rc = RunCbSEH(s.cb, context, srcObj, spawned);
    t_result = nullptr;
    t_frame = CallerFrame{nullptr, nullptr};
    t_inCb = false;
    if (rc != 0) {
        UE_LOGE("ufunction_hook: post-native cb AV absorbed (slot %d, ufn=%p src=%p result=%p) -- "
                "engine continues", N, s.ufunction, srcObj, spawned);
    }
}

template <int N>
void __fastcall PreThunk(void* context, void* stack, void* result) {
    Slot& s = g_slots[N];
    // Armed, a call off the game thread forwards untouched: the callbacks are game-thread code.
    if (!s.armed.load(std::memory_order_relaxed) || t_inCb || !stack || !ue_wrap::game_thread::IsGameThread()) {
        s.original(context, stack, result);
        return;
    }
    uint8_t*& code = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(stack) + P::off::FFrame_Code);
    uint8_t* const entry = code;
    alignas(16) uint8_t frame[kPreFrameBytes];
    const uint8_t* parms;
    if (entry) {
        // FFrame::Step once per parameter, the frame's own object as the context, as the original's
        // P_GET steps make them; the end-of-parameters opcode must then be next.
        std::memset(frame, 0, sizeof(frame));
        void* const frameObject = ReadPtr(stack, P::off::FFrame_Object);
        for (int i = 0; i < s.paramCount; ++i) {
            const uint8_t op = *code++;
            g_exec[op](frameObject, stack, frame + s.paramOffsets[i]);
        }
        if (*code != kExEndFunctionParms) {
            const unsigned found = *code;
            code = entry;   // the stream does not match the signature: the original steps it as ever
            if (!s.streamMismatchLogged) {
                s.streamMismatchLogged = true;
                UE_LOGE("ufunction_hook: pre slot %d (ufn=%p) stepped %d parameter(s) and found opcode "
                        "0x%02X, not the end of parameters -- forwarding such calls unrefused", N,
                        s.ufunction, s.paramCount, found);
            }
            s.original(context, stack, result);
            return;
        }
        parms = frame;
    } else {
        parms = static_cast<const uint8_t*>(ReadPtr(stack, P::off::FFrame_Locals));
    }
    void* srcObj = ReadPtr(stack, P::off::FFrame_Object);
    Verdict verdict = Verdict::Run;
    t_inCb = true;
    t_frame = CallerFrame{ReadPtr(stack, P::off::FFrame_Node),
                          static_cast<uint8_t*>(ReadPtr(stack, P::off::FFrame_Locals))};
    const int rc = RunPreCbSEH(s.pre, context, srcObj, parms, &verdict);
    t_frame = CallerFrame{nullptr, nullptr};
    t_inCb = false;
    if (rc != 0) {
        verdict = Verdict::Run;
        UE_LOGE("ufunction_hook: pre-native cb AV absorbed (slot %d, ufn=%p src=%p) -- the call runs", N,
                s.ufunction, srcObj);
    }
    if (verdict == Verdict::Cancel) {
        if (entry) ++code;   // P_FINISH: the original would have stepped over the end of parameters
        return;
    }
    if (entry) code = entry;
    s.original(context, stack, result);
}

// One distinct stamped thunk per slot, generated FROM kMaxNativeHooks -- the table can
// never under-enumerate the capacity (the old hand-written switch could, and its
// static_assert pinned the constant instead of following it). A slot takes the thunk of its
// kind, post or pre.
template <size_t... Is>
constexpr std::array<NativeFuncPtr, sizeof...(Is)> MakeThunkTable(std::index_sequence<Is...>) {
    return {{&NativeThunk<static_cast<int>(Is)>...}};
}
template <size_t... Is>
constexpr std::array<NativeFuncPtr, sizeof...(Is)> MakePreThunkTable(std::index_sequence<Is...>) {
    return {{&PreThunk<static_cast<int>(Is)>...}};
}
constexpr std::array<NativeFuncPtr, kMaxNativeHooks> g_thunkTable =
    MakeThunkTable(std::make_index_sequence<kMaxNativeHooks>{});
constexpr std::array<NativeFuncPtr, kMaxNativeHooks> g_preThunkTable =
    MakePreThunkTable(std::make_index_sequence<kMaxNativeHooks>{});

NativeFuncPtr ThunkFor(int n) {
    return (n >= 0 && n < kMaxNativeHooks) ? g_thunkTable[static_cast<size_t>(n)] : nullptr;
}
NativeFuncPtr PreThunkFor(int n) {
    return (n >= 0 && n < kMaxNativeHooks) ? g_preThunkTable[static_cast<size_t>(n)] : nullptr;
}

// The first pre hook installed on `ufunction`: every later pre wraps the Func above it, so it is
// the innermost, and a post hook installs under it to keep the pres outermost. Null when none.
Slot* InnermostPre(void* ufunction) {
    for (int i = 0; i < g_slotCount; ++i)
        if (g_slots[i].ufunction == ufunction && g_slots[i].pre) return &g_slots[i];
    return nullptr;
}

NativeFuncPtr* FuncSlotOf(void* ufunction) {
    return reinterpret_cast<NativeFuncPtr*>(reinterpret_cast<uint8_t*>(ufunction) + P::off::UFunction_Func);
}

// The parameters a pre hook steps, into `into`; false with one line naming the first one it cannot.
bool ReadPreSignature(void* ufunction, Slot& into) {
    int count = 0;
    for (const auto& p : R::FunctionParams(ufunction)) {
        const char* why = nullptr;
        const int32_t written = p.size > 4 ? p.size : 4;   // a bool is written as four bytes
        if (p.flags & P::cpf::ReturnParm) why = "a return value, which a refused call would leave unwritten";
        else if ((p.flags & P::cpf::OutParm) && (p.flags & kConstRef) != kConstRef)
            why = "an out parameter, or a reference that is not const";
        else if (!(p.flags & P::cpf::NoDestructor))
            why = "a parameter the engine destroys (a raw frame cannot)";
        else if (count >= kMaxPreParams || p.offset < 0 ||
                 p.offset + written + kPreFrameSlack > kPreFrameBytes)
            why = "past the forwarder's frame";
        if (why) {
            UE_LOGE("ufunction_hook: pre hook on ufn=%p refused: '%ls' (flags 0x%llX) is %s", ufunction,
                    p.name.c_str(), static_cast<unsigned long long>(p.flags), why);
            return false;
        }
        into.paramOffsets[count++] = static_cast<int16_t>(p.offset);
    }
    into.paramCount = count;
    return true;
}

}  // namespace

CallerFrame CurrentCallerFrame() { return t_frame; }

void* CurrentResult() { return t_result; }

bool SetArmed(void* ufunction, PostNativeCallback cb, bool armed) {
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].ufunction == ufunction && g_slots[i].cb == cb) {
            g_slots[i].armed.store(armed, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

bool SetArmed(void* ufunction, PreNativeCallback cb, bool armed) {
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].ufunction == ufunction && g_slots[i].pre == cb) {
            g_slots[i].armed.store(armed, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

namespace {

// The patch table's own entry points; the public forms take a resolved row (ufunction_hook.h).
bool InstallPostHookFn(void* ufunction, PostNativeCallback cb, bool armed) {
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
    // Under a pre hook the post goes below the innermost pre, which then forwards to it.
    Slot* pre = InnermostPre(ufunction);
    auto* funcSlot = pre ? &pre->original : FuncSlotOf(ufunction);
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
    g_slots[n].armed.store(armed, std::memory_order_relaxed);
    g_slotCount = n + 1;     // slot fully populated before the thunk can be reached
    // An 8-byte aligned pointer swap: the Func slot is 8-aligned and the UFunction lives in the
    // writable UE4 object pool. Atomic on x64, and game-thread-only dispatch means no torn read
    // in any case.
    *funcSlot = ThunkFor(n);
    UE_LOGI("ufunction_hook: patched ufn=%p Func @0x%zX (orig=%p -> thunk slot %d%s) -- standalone "
            "UFunction::Func hook (catches EX_CallMath calls invisible to ProcessEvent)",
            ufunction, static_cast<size_t>(P::off::UFunction_Func), original, n,
            pre ? ", under its pre hook" : "");
    return true;
}

bool InstallPreHookFn(void* ufunction, PreNativeCallback cb, bool armed) {
    if (!ufunction || !cb) return false;
    for (int i = 0; i < g_slotCount; ++i) {
        if (g_slots[i].ufunction == ufunction && g_slots[i].pre == cb) return true;
    }
    // A script function's Func is ProcessInternal, whose frame's code is the callee's own body, not parameters.
    const uint32_t flags = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(ufunction) +
                                                              P::off::UFunction_FunctionFlags);
    if (!(flags & P::off::FUNC_Native)) {
        UE_LOGE("ufunction_hook: pre hook on ufn=%p refused: not a native function (flags 0x%X)", ufunction, flags);
        return false;
    }
    const script_gate::ExecHandler* exec = script_gate::ExecHandlers();
    if (!exec) {
        UE_LOGE("ufunction_hook: pre hook on ufn=%p refused: no exec-handler table (script_gate did not "
                "resolve it)", ufunction);
        return false;
    }
    if (g_slotCount >= kMaxNativeHooks) {
        UE_LOGE("ufunction_hook: table full (%d slots) -- cannot patch ufn=%p (grow kMaxNativeHooks; "
                "the thunk table is generated from it)", kMaxNativeHooks, ufunction);
        return false;
    }
    const int n = g_slotCount;
    if (!ReadPreSignature(ufunction, g_slots[n])) return false;
    auto* funcSlot = FuncSlotOf(ufunction);
    NativeFuncPtr original = *funcSlot;
    if (!original) {
        UE_LOGE("ufunction_hook: ufn=%p Func @0x%zX reads null -- offset wrong for this build? NOT patching",
                ufunction, static_cast<size_t>(P::off::UFunction_Func));
        return false;
    }
    g_exec = exec;
    g_slots[n].ufunction = ufunction;
    g_slots[n].original  = original;
    g_slots[n].pre       = cb;
    g_slots[n].armed.store(armed, std::memory_order_relaxed);
    g_slotCount = n + 1;     // slot fully populated before the thunk can be reached
    *funcSlot = PreThunkFor(n);
    UE_LOGI("ufunction_hook: pre hook on ufn=%p Func @0x%zX (orig=%p -> pre thunk slot %d, %d parameter(s) "
            "stepped, %s)", ufunction, static_cast<size_t>(P::off::UFunction_Func), original, n,
            g_slots[n].paramCount, armed ? "armed" : "disarmed");
    return true;
}

}  // namespace

void DebugForEachHook(HookVisitor visit, void* ctx) {
    if (!visit) return;
    for (int i = 0; i < g_slotCount; ++i)
        if (g_slots[i].ufunction) visit(ctx, g_slots[i].ufunction, g_slots[i].pre != nullptr);
}

bool InstallPostHook(const facts::Resolved& fn, PostNativeCallback cb, bool armed) {
    if (!fn) return false;  // not resolved yet: nothing was asked, nothing is recorded
    const bool ok = InstallPostHookFn(fn.Fn(), cb, armed);
    fn.Row()->NoteUse(facts::kUseNativeHook, ok);
    return ok;
}

bool InstallPreHook(const facts::Resolved& fn, PreNativeCallback cb, bool armed) {
    if (!fn) return false;
    const bool ok = InstallPreHookFn(fn.Fn(), cb, armed);
    fn.Row()->NoteUse(facts::kUseNativeHook, ok);
    return ok;
}

}  // namespace ue_wrap::ufunction_hook
