// coop/dev/floppy_selftest.h -- the disc-into-server media transfer, driven (`[dev] floppy_selftest=1`).
//
// An idle two-peer run never moves a disc between a prop and a device, so the seams that carry a
// prop's own save data -- the keyed destroy an insert relays, the birth an eject drives -- stay
// invisible to it. This drives the game's own verbs on a per-role timer: a client insert whose
// host eject finds an empty slot, the mirror of that, and a client insert that the same client
// ejects, which is the only episode putting a disc through the client's own place/birth seam.
//
// Each episode records the slot and the world's disc census on BOTH peers around the verb, so a
// disc lost between them is a diff of two logs rather than an absence in one, and an episode that
// could not fire says which precondition stopped it instead of leaving a silent gap. It MUTATES
// the world -- it inserts, ejects, and seeds discs when the world has none -- so it is armed per
// run from the environment (VOTVCOOP_FLOPPY_SELFTEST) and never left standing in an ini.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::floppy_selftest {

// Cache the session pointer. Call once at boot. No-op with the flag off.
void Install(coop::net::Session* session);

// Game thread, per tick. Resolves the targets, fires the episodes due for this role, and prints
// the census on its own period. No-op with the flag off or before the session connects.
void Tick();

// Print what each episode did, including the ones that never fired. Called on the periodic census
// and at teardown, since the rig kills its peers rather than disconnecting them.
void EmitVerdict();

// Clear the schedule and the resolved targets so a reconnect re-runs the episodes.
void OnDisconnect();

}  // namespace coop::dev::floppy_selftest
