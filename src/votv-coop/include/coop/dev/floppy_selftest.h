// coop/dev/floppy_selftest.h -- the disc-into-server media transfer, driven
// (`VOTVCOOP_FLOPPY_SELFTEST=1` for one run).
//
// An idle two-peer run never moves a disc between a prop and a device, so the seams that carry a
// prop's own save data -- the keyed destroy an insert relays, the birth an eject drives -- stay
// invisible to it. This drives the game's own verbs, each when its state is true on the peer that
// acts -- inserts ejected across peers and by the same peer, then the laptop's three -- and each
// peer moves on when it SEES an episode's effect; one whose state never comes ends the run there,
// named. Each peer ends with `floppy_selftest: RESULT PASS|FAIL` and `floppy_selftest: DONE
// role=<ROLE>`, the client's last. The slot and the disc census are logged on BOTH peers around
// each verb, so a lost disc is a diff of two logs. It MUTATES the world -- inserts, ejects, seeds
// discs, OVERWRITES the content of every disc it names -- so it is armed per run from the
// environment and never left standing in an ini.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::floppy_selftest {

// Cache the session pointer. Call once at boot. No-op with the flag off.
void Install(coop::net::Session* session);

// Game thread, per tick. Resolves the targets, fires the episodes that are ready for this role,
// and prints the census on its own period. No-op with the flag off or before the session connects.
void Tick();

// Print what each episode did, including the ones that never fired. Called on the periodic census
// and at teardown, since the rig kills its peers rather than disconnecting them.
void EmitVerdict();

// Clear the episode cursor and the resolved targets so a reconnect re-runs the episodes.
void OnDisconnect();

}  // namespace coop::dev::floppy_selftest
