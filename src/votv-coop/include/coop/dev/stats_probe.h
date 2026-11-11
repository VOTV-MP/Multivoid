// coop/dev/stats_probe.h -- probe: every player stat read, written and restored in one frame, and
// the game's status effects added, listed and removed (ini stats_probe=on|red / env
// VOTVCOOP_STATS_PROBE; BOTH peers, each its own local player).
//
// Once per session, in one Tick call, each peer walks the 20 rows of ue_wrap::vitals in Field
// order (a writable row read, written, read back, restored, read again; a read-only row read),
// then checks the snapshot's seven members by name, the refusals and the class defaults. The
// effects leg then takes every row of the game's effect table: five are added, listed, removed
// and listed, three only listed by name, an unvetted one is a MISMATCH, an active one skipped.
// A stat write is restored inside the call, so the streams never carry a test value; an effect's
// removal takes the one instance an add makes, so an add that made two, or a failed list after an
// add, leaves one. `red` adds once but expects a wrong Health read-back and two bloodLoss entries,
// so it fails. A client starts at ClientReady; the host once one row of each owner reads.
// [STATS-PROBE]: one line per row, snapshot, refusals, defaults and effect, then 'DONE bad=<m>
// ok=<n>' (a green run with no active effect prints bad=0 ok=31).

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::stats_probe {

// Run the probe once the peer is ready. Every pump tick in a world; a latched read when off. Game
// thread.
void Tick(coop::net::Session* session);

// The session ended: the probe runs again in the next one.
void OnDisconnect();

}  // namespace coop::dev::stats_probe
