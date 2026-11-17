// coop/dev/atv_passenger_drill.h -- drill: the ATV's passenger seats on two peers (env VOTVCOOP_ATV_PASSENGER_DRILL=on with
// VOTVCOOP_JOIN_AT_HOST=1; BOTH peers; they sequence on lobby-chat lines). The host sets noon, spawns an ATV, logs its geometry (the
// seat offsets are measured from those lines) and drives it with the client aboard: the client boards through the real use key and
// holds seat 0 on both tables (its puppet attached to the host's copy), rides within a tolerance, stays when the driver leaves, and
// leaves beside the ATV on the ground. The host then rides the client's ATV the same way. A second boarding follows (left by the F1
// teleport), then the red arms, each a raw AtvPassengerRequest the host's counters judge: a duplicate board while seated (already_seated), a board with
// every seat taken (full; the host fills them with atv_authority::DrillSetPassengerGhosts) and a board out of reach (the host
// moves its ATV away). Lines tagged [ATVPASS-DRILL]; the host's PASS or FAIL line is the verdict.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::atv_passenger_drill {

// Every pump tick in a world; a latched read when off. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the drill starts over, a walk it started is cancelled.
void OnDisconnect();

}  // namespace coop::dev::atv_passenger_drill
