// ue_wrap/actors/prop_pry.h -- the game's pryable props, as the engine layer sees them: which class of prop a crowbar's
// pry reaches, the plank's own pry body (its flags cleared and the crowbar's kick) and the crowbar's right vector that
// body reads. Engine-wrapper layer (principle 7): no network or coop state. The lane that carries a pry between peers
// is coop/props/prop_pry_sync; the host's judge is coop/props/prop_pry_host.
// Implementation: src/ue_wrap/actors/prop_pry.cpp.

#pragma once

namespace ue_wrap::prop_pry {

// The pryable prop classes. A plank is prop_pryablePlank_C or a class below it (its edges-only variant); its crowbarOpen
// clears the prop's flags and kicks its body along the crowbar's right vector.
enum class Kind : unsigned char { None, Plank };

// The kind of pryable prop `obj` is: by class, down the super chain. None for a null or dead object, and for any other
// class, the base prop_C's empty crowbarOpen included. Game thread.
Kind KindOf(void* obj);

// The prying crowbar's right vector: its root component's GetRightVector, which a plank's crowbarOpen reads for its kick.
// False, `out` untouched, for a null or dead actor, a root that does not resolve or a call that did not run. Game thread.
bool ReadRightVector(void* crowbar, float out[3]);

// A plank's own pry on `prop`, the body of prop_pryablePlank_C::crowbarOpen without its sound: setPropProps(false x4),
// then the unit direction (x, y, z) times 150 deg/s added to the body's angular velocity. False when the prop is not a
// plank or a call did not run (said). Game thread.
bool FreePlank(void* prop, float x, float y, float z);

}  // namespace ue_wrap::prop_pry
