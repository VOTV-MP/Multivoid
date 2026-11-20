// coop/dev/clean_drill.h -- [dev] the grime lane's clean drill, two arms on three peers (the host and two clients, mp.py --clients
// 2), set on EVERY peer by the `clean_drill` row with VOTVCOOP_CAN_DIR on every peer. Each step is decided by an event, and a
// verdict is said the moment it is known: `rig: VERDICT grime-clean` and `rig: VERDICT grime-leaves`, PASS, FAIL or INVALID (a time
// bound can only say INVALID).
//   clean  -- the actor (the first client; VOTVCOOP_CLEAN_ACTOR=host: the host) cleans away one decal of each grime kind near the
//             save's bucket (VOTVCOOP_CLEAN_BUCKET names its key) and every peer says whether its copy ended; then the actor's
//             log-only legs (blood, the bucket's dip and its thrown water), and every peer's `[CLN] all` dumps of the grime it
//             holds before and after. Red: grime_gone_off.
//   leaves -- every peer runs the autumn leaf spawn the wind's timer runs; every peer must hold the host's fallen leaves at the
//             host's places and no leaf of its own. Red: leaves_lane_off.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::clean_drill {

// The dev lanes' tick. A latched read when the drill is off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::clean_drill
