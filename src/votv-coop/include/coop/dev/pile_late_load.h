// coop/dev/pile_late_load.h -- [dev] the late-load seam: one save-loaded clump of a joiner is kept aside from its
// saved pose until its row defers, so the deferral path runs on an artificial late load (ini pile_late_load=1 / env
// VOTVCOOP_PILE_LATE_LOAD, with pile_defer_drill=check|giveup; JOINER only; a cached bool read when off).
//
// Not a natural deferral: the first clump the join's load catch holds is moved 200 cm along +X (held, kinematic,
// tick off). Its row then misses at the key and defers in pile_spawn_bind. On the tick after the deferral, arm check
// puts the clump back at its saved pose, so the quiescence sweep finds it at the key and binds it; arm giveup
// destroys it, as a native that never loaded, so the sweep gives up and the host's clump materialises. One clump
// per session. Lines are tagged [PILE-LATE-LOAD]. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::dev::pile_late_load {

// The row is on and pile_defer_drill is armed; both read once.
bool On();

// CLIENT, trash_native_hold: the join's load catch held `clump`. The first one while On() is chosen and moved
// aside; the rest are left alone.
void NoteLoadCaught(void* clump);

// CLIENT, pile_spawn_bind: the bind of `eid` at `key` was deferred to the sweep. Marks the chosen clump's eid when
// the key is within 1 cm of its saved pose and the form is a clump.
void NoteDeferred(uint32_t eid, const ue_wrap::FVector& key, bool wantClump);

// The chosen clump's eid; 0 until its row has deferred.
uint32_t ChosenEid();
bool ChoseThisSession();  // a clump was kept aside in this session (the seam acts once per process)

// Once per pump tick; acts on the tick after the deferral.
void Tick();

// The session ended: a clump still aside goes back to its saved pose, and the choice is forgotten.
void OnDisconnect();

}  // namespace coop::dev::pile_late_load
