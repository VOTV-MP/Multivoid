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
bool Install(void* target, void* detour, void** original);

// Disable + remove the hook on `target`. Returns true on success.
bool Uninstall(void* target);

// Disable + remove all hooks and uninitialize. Safe to call once at shutdown.
void Shutdown();

}  // namespace ue_wrap::hook
