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
//   ue_wrap::hook::Install(target, &Detour, &g_original);  // create + enable
//   ...
//   ue_wrap::hook::Uninstall(target);                      // disable + remove

#pragma once

#include <cstdint>

namespace ue_wrap::hook {

// Initialize the hooking engine. Idempotent; returns true if ready.
bool Init();

// Create AND enable a hook on `target`. `detour` replaces it; `*original`
// receives the trampoline (call it to invoke the un-hooked target). Returns
// false on any MinHook error (logged). Both pointers must be non-null.
//
// `followJmpImmune` (WP-2, 2026-08-22): rewrite MinHook's x64 relay from the
// classic indirect `FF 25 [rip+0] + abs64` form to a non-branching-led
// `MOV RAX, imm64 ; JMP RAX` form BEFORE enabling. A co-resident inline-hook
// engine that follows jmp chains (UE4SS ships PolyHook, whose x64Detour::hook()
// runs followJmp) then STOPS on the MOV and cleanly in-place-hooks the relay
// itself instead of resolving our indirect jmp onto the abs64 pointer slot and
// clobbering it (the PROVEN WP-2 boot-crash root cause). Absolute-jump semantics
// are identical; only the encoding followJmp keys on changes. Pass true ONLY for
// a target another inline-hook engine also detours (ProcessEvent). Default false.
bool Install(void* target, void* detour, void** original, bool followJmpImmune = false);

// Disable + remove the hook on `target`. Returns true on success.
bool Uninstall(void* target);

// Disable the hook on `target` WITHOUT removing it: the patch is lifted (the
// detour stops firing) but the trampoline slot stays allocated, so a thread
// preempted inside the detour body still returns through live memory. Pair
// with Enable to re-arm later. This is the ONLY safe retirement for a detour
// other threads may be entering concurrently (an inflight counter cannot
// prove absence -- a thread can be preempted between the patched jump and the
// counter increment; see the DX12 overlay design 2026-07-26).
bool Disable(void* target);

// Re-enable a previously Disabled hook on `target`.
bool Enable(void* target);

// Disable + remove all hooks and uninitialize. Safe to call once at shutdown.
void Shutdown();

}  // namespace ue_wrap::hook
