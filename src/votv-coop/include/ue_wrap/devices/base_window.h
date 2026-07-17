// ue_wrap/base_window.h -- standalone engine access for VOTV's base observation
// window (AbaseWindow_C), the "main huge window" of the base. Principle-7 engine-
// wrapper layer: it wraps the reflection / struct-offset / UFunction-thunk details
// of a baseWindow actor. NO network logic, NO gameplay/coop state -- coop::window_sync
// owns those and talks to the engine through here.
//
// A baseWindow is an Aactor_save_C descendant. Its dirt is the WHOLE-SURFACE scalar
// `clean`@0x0260 (float; higher = dirtier, wiped DOWN to 0 by cleanSponge, never
// re-raised). setClean() pushes it into the mesh shader via SetCustomPrimitiveDataFloat(0).
// Its cross-peer-stable identity is the inherited Aactor_save_C::Key FName (assigned
// deterministically by intComs_gamemodeMakeKeys + save-persistent, the same Key family
// doors use). NOTE: unlike doors (AtriggerBase_C::Key@0x0260) the Key is at the
// Aactor_save_C offset (0x0230) -- FindPropertyOffset does NOT climb to parent classes,
// so EnsureResolved resolves it against actor_save_C explicitly.
//
// RE: research/findings/world-systems/votv-dirt-window-cleaning-RE-and-coop-sync-design-2026-06-07a.md.

#pragma once

#include <string>

namespace ue_wrap::base_window {

// Resolve the baseWindow_C UClass + the `clean` field offset + the inherited
// Aactor_save_C::Key offset + the setClean UFunction. Idempotent; returns true once
// everything resolved (false while the baseWindow_C BP class is not yet loaded -- the
// caller retries on a later tick, same wait-and-retry shape as the other EnsureResolved
// paths). Game thread.
bool EnsureResolved();

// The baseWindow_C UClass pointer (nullptr until EnsureResolved succeeds). Exposed for
// IsBaseWindow's descendant check.
void* BaseWindowClass();

// True iff `obj`'s class is baseWindow_C or a subclass. Cheap (a bounded SuperStruct
// walk; no allocation). False if not yet resolved.
bool IsBaseWindow(void* obj);

// Read the window's Aactor_save_C::Key FName as a wide string. Empty on failure
// (null / not resolved). An unkeyed window returns L"None".
std::wstring GetKeyString(void* win);

// Read the window's `clean` scalar into `out`. Returns false if the read could not be
// made (null window / not resolved); leaves `out` untouched on failure.
bool ReadClean(void* win, float& out);

// Write the window's `clean` scalar + call AbaseWindow_C::setClean() to push it into the
// shader (SetCustomPrimitiveDataFloat(0, clean) -- a pure setter, no gameplay side effect,
// game-thread). Returns false on null window / unresolved. The receiver of a remote wipe
// uses this; coop::window_sync echo-suppresses so the resulting field change isn't re-broadcast.
bool WriteCleanAndApply(void* win, float clean);

}  // namespace ue_wrap::base_window
