// coop/dev/constraint_join_drill.h -- drill: every prop a physics constraint ties to a driven prop gets a drive row of its
// own and converges on every peer (ini constraint_join_drill=off|c1|c2|c3 / env VOTVCOOP_CONSTRAINT_JOIN_DRILL; BOTH peers,
// one client; default off).
//
// The host builds a nailed set of prop boxes in a line on measured open ground, the boxes 1 cm apart
// (coop/dev/nail_act puts each nail through two neighbours as the nailgun's projectile does; a chain stands one box at a
// time, each nailed from its outer side into the box before it, before the next stands), and waits until every drive on
// it is closed; the client is told the set in chat lines. The bot director walks and grabs (coop/dev/director), never a
// teleport.
//   c1  a chain of five nailed in line: the host carries the end box A 300 cm away from the chain and lets go, so the far
//       members stand more than 150 cm from the host's body (past a body's push).
//   c2  a nailed pair: the host walks more than 5 m off, the CLIENT carries A 300 cm and lets go; then the client leaves and
//       rejoins (the rig's --rejoin; its second life is told by VOTVCOOP_CONSTRAINT_JOIN_LIFE=2).
//   c3  a chain of six with constraint_join_drill_cap=2: the host carries A; members past the cap are owed a row.

#pragma once

// Verdicts, each at its event (a time bound says INVALID and nothing else):
//   rig: VERDICT constraint-join PASS | FAIL -- <leg>: ... | INVALID -- ...
//   HOST    at the leader's End, every arm: each box but the leader had a row in this drive, opened by the join (the cause
//           the host keeps of each prop's last row, prop_drive_host::LastOpenCause), else FAIL at `leader-end` naming it
//           (body_push_off must be set, or the host says INVALID at the start; c3 first needs a member to have waited past the cap, else INVALID). c2 also at the client's let-go: B had a row
//           since the take, else FAIL at `let-go`. The spread of the boxes' End times is logged against two rest probes.
//   CLIENT  at each End applied to a member: its copy within 5 cm of the End pose, else FAIL at `client-end`. PASS when every
//           box's last End is the host's final pose (c1, c3); in c2, after the rejoin, when every copy stands within 5 cm of
//           the host's final pose (else FAIL at `rejoin`).
// Reds, each its own run, by constraint_join_off: join (c1) and owed (c3) FAIL at `leader-end`, hand (c2) at `let-go`.

namespace coop::net { class Session; }

namespace coop::dev::constraint_join_drill {

// True unless the row is `off`.
bool IsEnabled();

// Advance this peer's steps. Every pump tick in a world. Game thread.
void Tick(coop::net::Session* session);

// The session ended: back to the first step. A c2 host that has said its final lines keeps its state, since the rejoin is
// the arm.
void OnDisconnect();

}  // namespace coop::dev::constraint_join_drill
