// ue_wrap/actors/prop_pry.h -- the game's pryable props, as the engine layer sees them: which class of prop a crowbar's
// pry reaches, a plank's own pry body (its flags cleared and the crowbar's kick), the break effect and the joints of a
// radiator or a security TV, and the crowbar's right vector a plank's body reads. Engine-wrapper layer (principle 7): no
// network or coop state. The lane that carries a pry between peers is coop/props/prop_pry_sync; the host's judge is
// coop/props/prop_pry_host.
// Implementation: src/ue_wrap/actors/prop_pry.cpp.

#pragma once

namespace ue_wrap::prop_pry {

// The pryable prop classes. A plank is prop_pryablePlank_C or a class below it (its edges-only variant); its crowbarOpen
// clears the prop's flags and kicks its body along the crowbar's right vector. A hanging prop is radiator_C (the placed
// radiator_base1_C and radiator_base2_C are below it) or securityTv_C: classes of prop_asologoPiece_C whose crowbarOpen
// is its breakingSequence, which frees the body and breaks its four physics constraints one after another.
enum class Kind : unsigned char { None, Plank, Hanging };

// A hanging prop's joints: PhysicsConstraint1..4, numbered from 1.
inline constexpr int kJoints = 4;

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

// A hanging prop's break(): the break particles and the sound the prop makes coming off its wall. False for a prop that
// is not hanging or when the event did not dispatch. Game thread.
bool CallBreakEffect(void* prop);

// Which of `prop`'s joints, 1..kJoints, the component `constraint` is; 0 when it is none of them or `prop` is not a
// hanging prop. Game thread.
int JointOf(void* prop, void* constraint);

// BreakConstraint on `prop`'s joint `joint` (1..kJoints), and with `sound` the prop's audio_object played once, as its
// own loop does at each break. Not the loop's random angular velocity, which only the host's body takes. False when the
// joint does not resolve or the call did not run. Game thread.
bool BreakJoint(void* prop, int joint, bool sound);

}  // namespace ue_wrap::prop_pry
