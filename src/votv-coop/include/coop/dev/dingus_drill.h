// coop/dev/dingus_drill.h -- drill: a dingus (Maxwell) found on one peer is found on every peer (ini dingus_drill=off|find|
// pocket|loaded|join / env VOTVCOOP_DINGUS_DRILL; BOTH peers; the reds are the values of dingus_red).
//
// Each arm ends on one `rig: VERDICT dingus PASS|FAIL|INVALID` line, said by the host (coop/session/rig_verdict.h), and a
// FAIL is said at its own line: no later leg of the run executes. A bound or a read the instrument could not take (-1) says
// INVALID; a "found" read is `ReadFound == 1 and ReadMusicOn == 0`. The host spawns the targets near the client's puppet (no
// walk, no teleport); the peers pace each other with drill_chat lines, on the lane's own ordered lane.
//   find    the client grabs a prop_dingus2_C and the host's copy is found; the host grabs one and the client's is found.
//   pocket  the client grabs a dingus, lets go, holds it and drops it: the host's copy born from the drop is found at its
//           birth drain; the host holds and drops that copy, and the client's mirror of the host's drop is found at its bind
//           (its set holds the key only from the host's word); then the same for a dingus of the host's own.
//   loaded  the host spawns a dingus and marks it found as the game's load does; the client holds it with no grab and
//           drops it, and the host's copy born from the drop is found: the birth drain seeded the set from the loaded copy.
//   join    the host finds a dingus before the client is launched (the rig's client-after is the host's `join dingus
//           found` line), holds and drops it after the join, and the joiner's mirror of the drop is found at its bind.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::dingus_drill {

// Advance this peer's steps. Every pump tick in a world. Game thread. A latched enum read when off.
void Tick(coop::net::Session* session);

// The session ended: back to the first step.
void OnDisconnect();

}  // namespace coop::dev::dingus_drill
