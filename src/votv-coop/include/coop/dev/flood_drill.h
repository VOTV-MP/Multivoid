// coop/dev/flood_drill.h -- drill: the basement flood is the host's on every peer (ini flood_drill=off|burst|mend|far|join /
// env VOTVCOOP_FLOOD_DRILL; BOTH peers; the reds are the flood_state_off, flood_judge_off and flood_join_off rows).
//
// Each arm ends on one `rig: VERDICT flood-<arm> PASS|FAIL|INVALID` line (coop/session/rig_verdict.h): the host says PASS, a
// peer says FAIL where it decides, a bound or an unreadable world says INVALID. The peers pace each other with drill_chat
// lines: a FloodState goes before the chat line the host says after it on the one ordered channel, so a peer that reads
// the line has applied it. The client is walked by the director, never teleported.
//   burst  the host bursts its first pipe on its own addDamage; the client's pipes read the host's, and once the host's level
//          has risen a whole step its level reads the host's within kLevelTolUu.
//   mend   the burst, then the client walks to the pipe, takes a toolbox into its hand and runs toolboxFix; the host's judge
//          mends its pipe (decided at the judge's own count), and once its level has fallen the client reads it.
//   far    the burst, then the client, out of reach with a toolbox, sends a raw FloodIntent: refused at the target check.
//   join   the burst and the rise, read on the client, which leaves (the rig's --rejoin); the host bursts a second pipe once
//          the rejoined client's world is captured, and that client (VOTVCOOP_FLOOD_DRILL_REJOIN=1) must read it at its
//          world-ready: only the ready edge's replay carries it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::flood_drill {

// Advance this peer's steps. Every pump tick in a world. Game thread. A latched enum read when off.
void Tick(coop::net::Session* session);

// The session ended: back to the first step (the host's join arm remembers the burst).
void OnDisconnect();

}  // namespace coop::dev::flood_drill
