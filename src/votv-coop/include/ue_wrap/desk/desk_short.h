// ue_wrap/desk/desk_short.h -- the base computer's short circuit (analogDScreenTest_C::shortCircuit).
//
// A liquid on the desk shorts it: a grimeProjectile_C that strikes it and a sponge bucket's pour that overlaps it each
// call the desk's own shortCircuit, on the peer whose liquid it was. The event sets `shortCircuiting`, sparks 50 times
// at random points of the desk over about ten seconds, then switches the base to solar power
// (gamemode.powerControl.solar) with the power-down sound, and clears the flag; a call while the flag is set does
// nothing. The flag is the poll's edge, and the call is the only way in. Engine access only; game thread.

#pragma once

namespace ue_wrap::desk_short {

// The desk's `shortCircuiting`. False with nothing written when the desk or the field is not resolved.
bool ReadShortCircuiting(void* desk, bool& out);

// Runs the desk's own shortCircuit. False when the call did not dispatch.
bool CallShortCircuit(void* desk);

}  // namespace ue_wrap::desk_short
