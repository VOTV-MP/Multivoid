// ue_wrap/actors/prop_flags.h -- the Aprop_C physics-state flags and the game's own verbs that
// change them. Principle-7 engine-wrapper layer: no network or coop state.
//
// Four bools on the base class decide whether a prop simulates: Static, frozen and sleep, which
// init() folds into SetSimulatePhysics(!(static || frozen || sleep)), and removeWOrespawn, which
// the save round-trips. Every grab verb clears frozen and sleep on the grabbing machine
// (awakeUnfreeze); a mount, a slot or the toolgun sets them through the setter. The readers are
// plain memory reads at the offsets sdk_profile.h holds; the caller owns liveness. ue_wrap/actors/
// prop.h includes this header, so a prop caller needs one include.

#pragma once

namespace ue_wrap::prop {

// Aprop_C.propData.heavy, the data-driven lift-versus-drag flag (not a mass compare): true uses
// the physics constraint (heavyGrab), false the physics handle (grabHandle).
bool IsHeavy(void* prop);

// Aprop_C.Static; a static prop cannot be grabbed.
bool IsStatic(void* prop);

// Aprop_C.frozen, the BP-controlled freeze; a frozen prop does not move when grabbed.
bool IsFrozen(void* prop);

// Static and frozen written on a live prop, the wall-attachable stick and unstick mirror; the
// caller follows with an init() dispatch so the BP recomputes SetSimulatePhysics and collision
// from the flags (the single-player unstick shape). Null-safe; game thread.
void WriteStatic(void* prop, bool on);
void WriteFrozen(void* prop, bool on);

// The game's own unfreeze, the one a player's grab of a frozen prop runs (playerGrabbed_pre):
// frozen and sleep cleared, the prop detached and re-init()ed; static is left as it is. False if
// the verb did not resolve or dispatch. Game thread.
bool CallAwakeUnfreeze(void* prop);

// Aprop_C.sleep: true means physics-sleeping. Aprop_C::init sets
// SimulatePhysics(!(static || frozen || sleep)), so a save-loaded settled prop is non-simulating,
// and the snapshot mirrors it kinematic on the client. Aprop_C lineage only; the offset is a
// stray byte elsewhere.
bool IsSleeping(void* prop);

// Aprop_C.removeWOrespawn, one of the bools the save round-trips. Live Aprop_C only.
bool ReadRemoveWOrespawn(void* prop);

}  // namespace ue_wrap::prop
