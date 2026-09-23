// ue_wrap/actors/fire_extinguisher.h -- engine access for the fire extinguisher (prop_fireExt_C)
// and its wall mount (fireExtHolder_C). Principle-7 engine-wrapper layer: no network or coop state.
//
// The extinguisher is a prop with three members of its own: `life`, the charge (1 is full, and a
// second of spray or of thrust drains 1/60 of it), `firing`, the spray while it is in a hand, and
// `active`, the runaway thrust an impact over 5 or a fire's damage sets off. Its save record keeps
// `life` and `active`. A mount is a plain actor, neither a prop nor saved: its box's begin-overlap
// snaps an extinguisher that enters it onto the attach point and freezes it through the prop's own
// setPropProps, its end-overlap forgets it, and `fireExt` names the one it holds. Taking one off a
// mount is an ordinary grab of a frozen prop, which the game unfreezes before it grabs.
//
// A Blueprint class dies with the world that loaded it, so a class is known here by its name and
// a member's offset is read off the live class; the offsets are kept, being the same for one
// cooked class in every world.

#pragma once

namespace ue_wrap::fire_extinguisher {

// Resolve the two class names. Dispatches the string-to-name conversion, so call it from a
// top-level game-thread pass, never from inside a hook. Idempotent; true once resolved.
bool ResolveNames();

// True iff `obj` is an extinguisher (prop_fireExt_C or a subclass), or a mount. False before
// ResolveNames. Game thread.
bool IsExtinguisher(void* obj);
bool IsMount(void* obj);

// fireExtHolder_C::fireExt, the extinguisher the mount holds, or null. The pointer is the
// Blueprint's own and is not liveness-checked here: compare it, do not dereference it. Null for
// anything that is not a mount. Game thread.
void* MountedExtinguisher(void* mount);

// The extinguisher's own members; false, with `out` untouched, for anything that is not one.
// Game thread.
bool ReadCharge(void* ext, float& out);
bool ReadSpraying(void* ext, bool& out);
bool ReadThrusting(void* ext, bool& out);

}  // namespace ue_wrap::fire_extinguisher
