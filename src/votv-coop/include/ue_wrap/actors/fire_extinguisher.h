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

#include "ue_wrap/core/types.h"

namespace ue_wrap::fire_extinguisher {

// Resolve the two class names. Dispatches the string-to-name conversion, so call it from a
// top-level game-thread pass, never from inside a hook. Idempotent; true once resolved.
bool ResolveNames();

// True once ResolveNames has succeeded. Game thread.
bool NamesResolved();

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

// The world rotation of the extinguisher's own component `member` ("PhysicsThruster", the thruster whose force is along its
// own -X axis, or "Arrow", the nozzle), read off the instance by reflection. False, `out` untouched, for anything that is not
// an extinguisher, a member that does not resolve or a component that does not read. Game thread.
bool ReadComponentRotation(void* ext, const wchar_t* member, FRotator& out);

// The extinguisher's `life`, `firing` and `active` written in place, with no call and none of the game's own side effects
// (a hand mirror's display, a client copy's thrust display, a charge the host counted). False, nothing written, for
// anything that is not an extinguisher or a member that does not resolve. Game thread.
bool WriteCharge(void* ext, float life);
bool WriteSpraying(void* ext, bool on);
bool WriteThrusting(void* ext, bool on);

// Whether the extinguisher's PhysicsThruster is active: the thruster's own state, which the game's `shoot` turns on, so a
// copy whose own `shoot` ran reads true and one that only shows the thrust reads false. False, `out` untouched, when it
// does not read. Game thread.
bool ReadThrusterOn(void* ext, bool& out);

// A member component of the extinguisher by its property name: FireExtSprayEffectProp, FireExtThrustEffectProp,
// FireExtThrustSoundProp or FireExtThrusterProp (ue_wrap/core/sdk_profile_names.h). Null for another name, for anything that
// is not an extinguisher, and for a member that is unset or not live. The offset is kept per class. Game thread.
void* Member(void* ext, const wchar_t* name);

// The game's own extinguishing ray, `extinguishRay(start, end, radius)`: a sphere trace that puts out what it touches in
// this peer's world. It never reads who holds the extinguisher, and a direct call never runs the extinguisher's tick.
// False when the call does not run. Game thread.
bool CastExtinguishRay(void* ext, const FVector& start, const FVector& end, float radius);

// The game's own runaway verb, `fireDamage(0)`: it never reads its argument and keeps its own checks (an extinguisher that is
// already `active` or being destroyed does nothing), so a thrust starts only if the game would start it. False when the call
// does not run; true is not a thrust. Game thread.
bool StartRunaway(void* ext);

}  // namespace ue_wrap::fire_extinguisher
