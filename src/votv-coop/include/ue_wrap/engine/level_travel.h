// ue_wrap/engine/level_travel.h -- the level-travel seam (UGameplayStatics::OpenLevel).
//
// Engine-wrapper layer (principle 7): NO gameplay logic, NO coop state, NO network. This
// module owns exactly one thing -- a MinHook detour on the single native function that
// unloads the world -- and offers a callback so a gameplay module can REFUSE a travel.
// Who refuses, and why, is not this file's business (docs/DEATH_ARC.md section 3 is).
//
// WHY A SEAM EXISTS HERE AT ALL. VOTV's death chain is a chain of BP-internal hops
// (`kill` -> `ragdollMode` -> `fallen` -> uber `dead := true` -> two RetriggerableDelays ->
// `lib_C::loadLevel('menu')` -> `mainGamemode::transition`), and every one of them is
// EX_LocalVirtualFunction / EX_Context -- invisible to our ProcessEvent detour per
// docs/COOP_DISPATCH_VISIBILITY.md. The chain's LAST hop, `OpenLevel`, is native. It is
// the only interceptable point on the whole chain, which is why the arc cuts there and why
// no bytecode patch is required anywhere in it.
//
// AND THE LAST HOP IS ALSO THE ONLY *LEGAL* ONE. The two RetriggerableDelays are armed
// timers that never re-read `dead`; while they are pending nothing can undo the death
// (clearing the flag cancels no timer). By the time `OpenLevel` is reached they have BOTH
// FIRED AND BEEN CONSUMED -- the death is finished rather than in flight, and what it left
// behind is ordinary mutable state. See [[lesson-a-latent-chain-cannot-be-undone-by-
// clearing-its-flag]].

#pragma once

#include <cstdint>

namespace ue_wrap::engine::level_travel {

// The veto. Return TRUE to CANCEL this travel (the original is not called, so
// UEngine::SetClientTravel never runs and no travel is ever requested); FALSE to let it
// through unchanged.
//
// THE CONTRACT IS "DATA ONLY", AND IT IS LOAD-BEARING, NOT STYLISTIC. This callback runs
// inside a native function called from the BP VM, so a UFunction dispatch made from here
// re-enters our own ProcessEvent detour: `pe_detour.cpp:381` fires every interceptor (one
// returning true would SKIP the very verb you called), `:388` every PRE-observer, and the
// post-observers follow -- all inside the detoured OpenLevel frame. That is the shape
// `[[lesson-vm-bracket-zero-engine-mid-verb]]` measured corrupting kerfur_convert. So a
// veto may read cached offsets, atomics and GUObjectArray slots, and MUST NOT dispatch a
// UFunction, allocate through the engine, or take a lock a game-thread task can hold.
// Whatever work the decision implies belongs on the next pump task, not here.
//
// `levelName` is the FName passed by value (8 bytes: {ComparisonIndex, Number}), handed
// through raw so a veto can use it as a second discriminator WITHOUT calling
// FName::ToString -- which would be a dispatch, and is therefore forbidden above.
using VetoFn = bool (*)(void* worldContextObject, uint64_t levelName, bool bAbsolute);

// Resolve UGameplayStatics::OpenLevel by AOB (profile::kSigOpenLevel) and install the
// detour. Idempotent; returns true once armed. A signature miss is logged ONCE and
// latched -- it means the signature is stale for this build, and retrying cannot help.
//
// Installing is separate from vetoing on purpose: with no veto set the detour is a pure
// pass-through, so arming it early costs one predictable branch on a function that fires
// at most a handful of times in a session.
bool Install();

bool IsInstalled();

// Publish (or clear, with nullptr) the veto. Thread-safe; takes effect on the next call.
void SetVeto(VetoFn fn);

// How many travels this process has refused. Diagnostics only.
uint64_t VetoCount();

// How many travels have passed through the detour, refused or not. Diagnostics only; the
// pair is what tells "the seam never fired" apart from "the seam fired and declined".
uint64_t SeenCount();

}  // namespace ue_wrap::engine::level_travel
