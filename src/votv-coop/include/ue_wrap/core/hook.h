// ue_wrap/hook.h -- minimal inline-hook wrapper (MinHook).
//
// Engine-wrapper layer (principle 7): no gameplay/network logic. The standalone
// mod owns its own function hooking (RULE No.3 -- no UE4SS at runtime). MinHook
// (an established library, MIT) provides the x64 trampoline machinery; this is a
// thin, RAII-free C++ facade so the rest of ue_wrap never touches MinHook types
// directly, which keeps the substrate swappable.
//
//   ue_wrap::hook::Init();
//   ue_wrap::hook::Install(target, &Detour, &g_xTrampoline);  // create + enable
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
// memory holding `target`'s stolen prologue plus a jump back into it. Calling it
// invokes the un-hooked target, but it is NOT the target's address and NOT in the
// target's module: it is memory MinHook can OVERWRITE OR UNMAP, and it stays valid
// only because nothing here asks MinHook to release it. The parameter is
// `trampoline` and never `original`, because MinHook's out-param IS the trampoline
// (`minhook/src/hook.c:634`) and `original` invites the reading that it aims at the
// engine's own ProcessEvent -- a reader who believes that clears a real
// use-after-free as safe. Returns false on any MinHook error (logged).
//
// Pass `followJmpImmune` true ONLY for a target another inline-hook engine also
// detours (ProcessEvent): it rewrites the relay so a jmp-following engine composes
// with us instead of clobbering it. docs/architecture.md has the encodings.
bool Install(void* target, void* detour, void** trampoline, bool followJmpImmune = false);

// ---- Retirement -------------------------------------------------------------
//
// Disable is the ONLY retirement this facade offers, and the absence of a
// remove/uninitialize counterpart fixes a live use-after-free. A MinHook
// `MEMORY_SLOT` is a UNION of a `pNext` link and the trampoline bytes
// (`minhook/src/buffer.c:43-50`), so `FreeBuffer` writes eight bytes AT OFFSET 0
// OF THE TRAMPOLINE, over the stolen prologue (`buffer.c:282`), from
// `MH_RemoveHook` (`hook.c:702`) -- in place, with no drain window that helps,
// and a thread still holding the pointer runs a list link as code. Disable only
// writes the original prologue back, so a thread already inside the detour
// returns through intact memory. The full account is docs/architecture.md.

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
// unmapped while the process is still running -- our own teardown runs a full
// 3 seconds before DLL_PROCESS_DETACH.
void Shutdown();

}  // namespace ue_wrap::hook
