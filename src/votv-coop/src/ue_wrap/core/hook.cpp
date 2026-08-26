#include "ue_wrap/core/hook.h"

#include "ue_wrap/core/log.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ue_wrap::hook {
namespace {

// The FACADE's armed state -- NOT "MinHook is initialized". Shutdown clears this
// and deliberately leaves MinHook itself initialized, because uninitializing frees
// trampolines the process is still calling through (hook.h, "Retirement"). One flag,
// not two: a second `g_retired` could disagree with this one, and then neither is
// authority. Ordering is load-bearing -- Shutdown clears it BEFORE lifting patches,
// so Enable's post-enable re-read cannot miss a teardown that started mid-call.
std::atomic<bool> g_live{false};

const char* StatusName(MH_STATUS s) { return MH_StatusToString(s); }

// WP-2 (2026-08-22): follow-jmp-immune relay rewrite -- the root-cause fix for
// the UE4SS-lane boot crash. On x64 MinHook ALWAYS routes a patched target
// through a relay (`FF 25 [rip+0]` + abs64 detour) that lives inside the 64-byte
// trampoline slot (hook.c:607 `pHook->pDetour = ct.pRelay`, unconditional).
// When a co-resident inline-hook engine that FOLLOWS jmp chains (UE4SS ships
// PolyHook, x64Detour::hook() -> followJmp) hooks the SAME function after us, it
// takes our target's `E9` into this relay, sees the indirect `FF 25` (a branch
// WITH displacement), resolves getDestination to the OPERAND effective address
// (the relay's abs64 POINTER slot), and writes its own target-patch THERE --
// clobbering &detour. Our relay then `jmp qword [rip]`s through a garbage pointer
// to a non-canonical address -> #GP (surfaced by Windows as "AV read -1").
// PROVEN from a full -fullcrashdump decode.
//
// Fix: rewrite the relay's LEADING instruction to a NON-branching form
// (`MOV RAX, imm64 ; JMP RAX`). followJmp stops on the MOV (PolyHook
// ADetour.cpp:66 `if (!front().isBranching()) return true;`), so PolyHook does a
// clean in-place hook of the relay itself and BOTH detours chain
// (PE -> our E9 -> relay -> PolyHook jmp -> UE4SS dispatch -> PolyHook trampoline
// = `mov rax,&ourDetour; jmp rax` -> our detour -> our MinHook trampoline ->
// real PE). Source-traced through PolyHook's VALLOC2 path. Absolute-jump
// semantics identical; only the byte encoding followJmp keys on changes.
//
// Safe because it runs BEFORE MH_EnableHook: the target is still unpatched, so
// nothing is executing the relay yet -- the rewrite races no thread. Fail-closed:
// if the expected `FF 25 00 00 00 00 <&detour>` signature is not found in the
// slot, leave it untouched (MinHook layout changed -> surface it, do not guess).
bool MakeRelayFollowJmpImmune(void* trampoline, void* detour) {
    if (!trampoline || !detour) return false;
    auto* base = static_cast<uint8_t*>(trampoline);
    const uint64_t want = reinterpret_cast<uint64_t>(detour);
    uint8_t* relay = nullptr;
    // The relay lives at pTrampoline+newPos inside the 64-byte MEMORY_SLOT; scan
    // for the classic MinHook relay signature whose abs64 target is OUR detour.
    for (int off = 0; off + 14 <= 64; ++off) {
        if (base[off] == 0xFF && base[off + 1] == 0x25 && base[off + 2] == 0x00 &&
            base[off + 3] == 0x00 && base[off + 4] == 0x00 && base[off + 5] == 0x00) {
            uint64_t p = 0;
            std::memcpy(&p, base + off + 6, sizeof(p));
            if (p == want) { relay = base + off; break; }
        }
    }
    if (!relay) {
        UE_LOGE("hook: immune-relay: FF25 relay not found in trampoline slot -- "
                "MinHook layout changed? leaving relay as-is (fail-closed)");
        return false;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(relay, 14, PAGE_EXECUTE_READWRITE, &oldProt)) {
        UE_LOGE("hook: immune-relay: VirtualProtect(RWX) failed on relay %p", relay);
        return false;
    }
    uint8_t buf[14];
    buf[0] = 0x48; buf[1] = 0xB8;                 // mov rax, imm64
    std::memcpy(buf + 2, &want, sizeof(want));    //   = &detour
    buf[10] = 0xFF; buf[11] = 0xE0;               // jmp rax
    buf[12] = 0x90; buf[13] = 0x90;               // pad to the 14-byte relay footprint
    std::memcpy(relay, buf, sizeof(buf));
    DWORD tmp = 0;
    VirtualProtect(relay, 14, oldProt, &tmp);
    FlushInstructionCache(GetCurrentProcess(), relay, 14);
    UE_LOGI("hook: immune-relay: relay @%p rewritten to MOV RAX,&detour/JMP RAX "
            "(followJmp-immune)", relay);
    return true;
}

}  // namespace

bool Init() {
    if (g_live) return true;
    const MH_STATUS s = MH_Initialize();
    // MH_ERROR_ALREADY_INITIALIZED is SUCCESS here, not an error. Shutdown clears
    // g_live without uninitializing MinHook (see hook.h "Retirement"), so the two
    // states legitimately disagree after a teardown and MinHook is the one telling
    // the truth about its own heap.
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        UE_LOGE("hook: MH_Initialize failed (%s)", StatusName(s));
        return false;
    }
    g_live = true;
    UE_LOGI("hook: MinHook initialized%s",
            s == MH_ERROR_ALREADY_INITIALIZED ? " (already; re-arming the facade)" : "");
    return true;
}

bool Install(void* target, void* detour, void** trampoline, bool followJmpImmune) {
    if (!g_live && !Init()) return false;
    if (!target || !detour || !trampoline) {
        UE_LOGE("hook: Install called with null target/detour/trampoline");
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, trampoline);
    if (s != MH_OK) {
        UE_LOGE("hook: MH_CreateHook(%p) failed (%s)", target, StatusName(s));
        return false;
    }
    // WP-2: make the relay followJmp-immune while the target is still unpatched
    // (before enable = thread-safe). `*trampoline` is the slot base; the relay
    // lives inside it. Best-effort: a failure is logged and non-fatal (the
    // classic relay still works absent a co-resident jmp-following hook engine).
    if (followJmpImmune) {
        MakeRelayFollowJmpImmune(*trampoline, detour);
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        UE_LOGE("hook: MH_EnableHook(%p) failed (%s)", target, StatusName(s));
        // THE ONE LEGITIMATE MH_RemoveHook IN THIS PROCESS, and the gate
        // (tools/hooks/minhook_free_gate.ps1) allowlists exactly this line.
        // Removing frees the trampoline, which is a use-after-free anywhere the
        // hook is live -- but ENABLE JUST FAILED, so the target was never patched
        // and no thread can be inside the trampoline or holding a pointer into it.
        // Leaving a created-but-disabled hook behind would leak the slot instead.
        MH_RemoveHook(target);
        return false;
    }
    UE_LOGI("hook: installed on %p (trampoline %p)", target, *trampoline);
    return true;
}

bool Disable(void* target) {
    if (!g_live || !target) return false;
    const MH_STATUS s = MH_DisableHook(target);
    if (s != MH_OK) {
        UE_LOGW("hook: MH_DisableHook(%p) (%s)", target, StatusName(s));
        return false;
    }
    UE_LOGI("hook: disabled %p (trampoline slot retained on purpose)", target);
    return true;
}

bool Enable(void* target) {
    if (!g_live || !target) return false;
    const MH_STATUS s = MH_EnableHook(target);
    if (s != MH_OK) {
        UE_LOGW("hook: MH_EnableHook(%p) re-arm (%s)", target, StatusName(s));
        return false;
    }
    // COMPARE-AFTER-ACT. The guard above is check-then-act on its own: this is
    // reachable from the RENDER thread (overlay_backend_dx12 -> dx12_capture::Rearm)
    // while the game thread is inside Shutdown, so a teardown can begin between the
    // guard and MH_EnableHook and we would re-arm a patch Shutdown had just lifted.
    // Shutdown clears g_live BEFORE its blanket disable, so re-reading it here catches
    // every interleaving: either we see the clear and lift our own patch, or Shutdown's
    // blanket runs after our enable and lifts it. Both orders end disabled.
    // Lock-free on purpose -- Shutdown is reachable from DLL_PROCESS_DETACH under the
    // loader lock, where a mutex owned by a thread Windows already killed never unlocks.
    if (!g_live) {
        MH_DisableHook(target);
        UE_LOGW("hook: re-arm of %p raced Shutdown -- lifted again (teardown wins)", target);
        return false;
    }
    UE_LOGI("hook: re-enabled %p", target);
    return true;
}

void Shutdown() {
    if (!g_live.exchange(false)) return;   // one-way; also the Enable race's answer
    // Clearing g_live FIRST is the whole ordering contract (see Enable): a re-arm that
    // slips past its own guard re-reads the flag after MinHook returns and undoes itself.
    //
    // Lift every patch, free NOTHING. No MH_RemoveHook, no MH_Uninitialize: both free
    // trampoline slots, and `[V]` minhook/src/buffer.c:282 writes a linked-list pointer
    // over the slot's first eight bytes as it does so -- over the stolen prologue a
    // thread may be about to return through. Measured 2026-08-26: this runs a full 3
    // seconds before DLL_PROCESS_DETACH, so "the process is dying" does not close that
    // window. The OS reclaims the slots at exit; there is nothing to buy by freeing.
    //
    // KNOWN RESIDUAL, pre-existing and unchanged here: MH_DisableHook(MH_ALL_HOOKS)
    // reaches MinHook's Freeze, which calls CreateToolhelp32Snapshot + SuspendThread
    // (`[V]` minhook/src/hook.c:267,348). On the DLL_PROCESS_DETACH path that runs under
    // the loader lock, where a toolhelp snapshot is a documented deadlock risk. It has
    // never been observed, and it is not what this change is fixing -- but it is real,
    // and the graceful-close path does not exercise it (the wndproc latches shutdown
    // first, so DllMain's call is the idempotent no-op). Filed in docs/UE4SS_ARC.md 4c.
    MH_DisableHook(MH_ALL_HOOKS);
    UE_LOGI("hook: all patches lifted (trampolines retained -- MinHook stays initialized)");
}

}  // namespace ue_wrap::hook
