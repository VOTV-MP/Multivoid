// ue_wrap/hook.h -- minimal inline-hook wrapper (MinHook).
//
// Engine-wrapper layer (principle 7): no gameplay/network logic. The standalone
// mod owns its own function hooking (RULE No.3 -- no UE4SS at runtime). MinHook
// (WP13: established library, MIT) provides the x64 trampoline machinery; this
// is a thin, RAII-free C++ facade so the rest of ue_wrap never touches MinHook
// types directly (keeps the substrate swappable, like reflection's sig_scan).
//
// Usage:
//   ue_wrap::hook::Init();
//   ue_wrap::hook::Install(target, &Detour, &g_xTrampoline);  // create + enable
//   ...
//   ue_wrap::hook::Disable(target);                           // lift the patch
//
// THERE IS NO REMOVE. See "Retirement" below -- that is the whole point of this
// header, and it is load-bearing, not stylistic.

#pragma once

#include <cstdint>

namespace ue_wrap::hook {

// Initialize the hooking engine. Idempotent; returns true if ready.
bool Init();

// Create AND enable a hook on `target`. `detour` replaces it.
//
// `*trampoline` receives MINHOOK'S TRAMPOLINE SLOT -- 64 bytes of MinHook-owned
// memory holding `target`'s stolen prologue plus a jump back into it. Calling
// through it invokes the un-hooked target, which is why callers store it and
// call it; but it is NOT the target's address and it is NOT in the target's
// module. It is memory MinHook can OVERWRITE OR UNMAP, and the only reason it
// stays valid is that nothing in this facade ever asks MinHook to release it.
//
// The parameter is named `trampoline` and not `original` because a 2026-05-27
// audit cleared a real use-after-free by reading the name `g_originalPE` and
// concluding the pointer aimed at the engine's ProcessEvent -- "a process-
// lifetime entry point that is never unloaded". `[V]` `minhook/src/hook.c:634`:
// `*ppOriginal = pHook->pTrampoline`. The name was the evidence the audit used,
// and the name was wrong. Do not reintroduce one that invites the same reading.
//
// Returns false on any MinHook error (logged). All three pointers must be
// non-null.
//
// `followJmpImmune` (WP-2, 2026-08-22): rewrite MinHook's x64 relay from the
// classic indirect `FF 25 [rip+0]` + abs64 form to a non-branching-led
// `MOV RAX, imm64 ; JMP RAX` form BEFORE enabling. A co-resident inline-hook
// engine that follows jmp chains (UE4SS ships PolyHook, whose x64Detour::hook()
// runs followJmp) then STOPS on the MOV and cleanly in-place-hooks the relay
// itself instead of resolving our indirect jmp onto the abs64 pointer slot and
// clobbering it (the PROVEN WP-2 boot-crash root cause). Absolute-jump semantics
// are identical; only the encoding followJmp keys on changes. Pass true ONLY for
// a target another inline-hook engine also detours (ProcessEvent). Default false.
bool Install(void* target, void* detour, void** trampoline, bool followJmpImmune = false);

// ---- Retirement -------------------------------------------------------------
//
// Disable is the ONLY retirement this facade offers, and the absence of a
// remove/uninitialize counterpart is the fix for a live use-after-free rather
// than a matter of taste.
//
// `[V]` `minhook/src/buffer.c:43-50` -- a `MEMORY_SLOT` is a UNION of a `pNext`
// link and the trampoline bytes. `[V]` `buffer.c:282` -- `FreeBuffer` does
// `pSlot->pNext = pBlock->pFree`, i.e. it writes eight bytes AT OFFSET 0 OF THE
// TRAMPOLINE, straight over the stolen prologue, and then `VirtualFree`s the
// whole block if it was the last slot in use. `[V]` `hook.c:702` -- the caller
// of `FreeBuffer` is `MH_RemoveHook`. So removing a hook CORRUPTS its trampoline
// IMMEDIATELY and IN PLACE; there is no window to drain and no sleep that helps,
// because the damage is already done when the call returns. Any thread holding
// the trampoline pointer and calling it afterwards executes a linked-list
// pointer as code.
//
// Disable, by contrast, only writes the target's original prologue back: new
// callers stop reaching us, while a thread already inside the detour still
// returns through live, intact memory. That asymmetry is why an in-flight
// counter cannot substitute -- a thread can be preempted between the patched
// jump and the counter increment, so absence is not provable, only irrelevant.
//
// The process is going to exit anyway; the OS reclaims every trampoline. There
// is nothing to buy by freeing and a use-after-free to pay for it.

// Disable the hook on `target`: the patch is lifted (the detour stops firing)
// but the trampoline slot stays allocated and intact. Pair with Enable to
// re-arm later.
bool Disable(void* target);

// Re-enable a previously Disabled hook on `target`.
//
// Re-reads the facade's live flag AFTER MinHook has re-armed the patch and
// lifts it again if Shutdown ran in between -- a check before the enable would
// be check-then-act, and this is reachable from the render thread
// (overlay_backend_dx12 -> dx12_capture::Rearm) while the game thread is in
// Shutdown. Deliberately lock-free: Shutdown is reachable from
// DLL_PROCESS_DETACH under the loader lock, where a mutex held by a thread
// Windows has already terminated would hang the process forever.
bool Enable(void* target);

// Lift every patch this process installed. Call once at process exit.
//
// Disable-only, for the reason spelled out under "Retirement" above: nothing is
// removed and MinHook is never uninitialized, so no trampoline is corrupted or
// unmapped while the process is still running (measured 2026-08-26: our own
// teardown runs a full 3 seconds before DLL_PROCESS_DETACH).
void Shutdown();

}  // namespace ue_wrap::hook
