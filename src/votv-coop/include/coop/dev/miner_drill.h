// coop/dev/miner_drill.h -- drill: the GPU mining rig (ini miner_drill=off|use|claim|record|join / env VOTVCOOP_MINER_DRILL; BOTH peers).
//
// use: a client's use of a GPU on a rig, judged by the host (miner_drill_use.cpp): the honest use, a forged use with an empty
// hand, a use on a full rig whose refused GPU is given back, and a flood of asks each with a hold behind it. claim: a client's claim of a rig's points, run by the host, which pays
// its balance (miner_drill_claim.cpp). record: a client's forged save record of a world rig, refused, and the record of its own
// honest drop, which lands (miner_drill_record.cpp). join: a rig built before the client joins, shown to it by the host's
// snapshot (miner_drill_join.cpp; the host starts first and the client joins after its marker line). The host says
// `rig: VERDICT miner-use|miner-claim|miner-record|miner-join PASS|FAIL|INVALID`; a client reports what it read in a chat line.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::miner_drill {

// Advance this peer's steps. Every pump tick in a world. Game thread. A latched enum read when off.
void Tick(coop::net::Session* session);

// The session ended: back to the first step.
void OnDisconnect();

}  // namespace coop::dev::miner_drill
