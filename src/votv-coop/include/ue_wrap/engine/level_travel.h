// ue_wrap/engine/level_travel.h -- the level-travel seam (UGameplayStatics::OpenLevel).
//
// Engine-wrapper layer (principle 7): NO gameplay logic, NO coop state, NO network. It owns one
// thing -- a MinHook detour on the single native function that unloads the world -- and offers a
// callback so a gameplay module can REFUSE a travel. Who refuses, and why, is docs/players.md.
//
// WHY A SEAM EXISTS HERE AT ALL. VOTV's death chain is BP-internal hops -- `kill`, `ragdollMode`,
// `fallen`, uber `dead := true`, two RetriggerableDelays, `lib_C::loadLevel('menu')`,
// `mainGamemode::transition` -- and every one of them is EX_LocalVirtualFunction or EX_Context,
// invisible to our ProcessEvent detour per docs/coop-dispatch-visibility.md. The chain's LAST hop,
// OpenLevel, is native and therefore the only interceptable point on it, which is why no bytecode
// patch is needed anywhere in the chain. It is also the only LEGAL point: the RetriggerableDelays
// are armed timers that never re-read `dead`, so while they pend nothing can undo the death --
// clearing the flag cancels no timer -- and by OpenLevel both have fired and been consumed.

#pragma once

#include <cstdint>

namespace ue_wrap::engine::level_travel {

// The veto. TRUE CANCELS this travel: the original is not called, so UEngine::SetClientTravel
// never runs and no travel is ever requested. FALSE lets it through unchanged.
//
// THE CONTRACT IS "DATA ONLY", AND IT IS LOAD-BEARING, NOT STYLISTIC. This callback runs inside a
// native function called from the BP VM, so a UFunction dispatch made from here re-enters our own
// ProcessEvent detour: FireInterceptors runs every interceptor -- one returning true would SKIP the
// very verb you called -- FirePreObservers then runs every PRE-observer, and the post-observers
// follow, all inside the detoured OpenLevel frame -- which is how a dispatch from here corrupts a
// verb already in flight. So a veto may read cached offsets, atomics and GUObjectArray
// slots, but MUST NOT dispatch a UFunction, allocate through the engine, or take a game-thread
// lock; work the decision implies belongs on the next pump task.
//
// `levelName` is the FName passed by value (8 bytes: {ComparisonIndex, Number}), handed through raw
// so a veto can use it as a second discriminator WITHOUT calling FName::ToString, which would be a
// dispatch and is therefore forbidden above.
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
