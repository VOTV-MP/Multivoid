// coop/dev/clean_drill.h -- [dev] the grime lane's clean drill, two arms on three peers (the host and two clients, mp.py --clients
// 2), set on EVERY peer by the `clean_drill` row with VOTVCOOP_CAN_DIR on every peer. Each step is decided by an event, and a
// verdict is said the moment it is known: `rig: VERDICT grime-clean` and `rig: VERDICT grime-leaves`, PASS, FAIL or INVALID (a time
// bound can only say INVALID).
//   clean  -- the actor (the first client; VOTVCOOP_CLEAN_ACTOR=host: the host) cleans away one decal of each grime kind near the
//             save's bucket (VOTVCOOP_CLEAN_BUCKET names its key) and every peer says whether its copy ended, and dumps the
//             grime it holds before and after (`[CLN] all`). Red: grime_gone_off.
//   leaves -- every peer runs the autumn leaf spawn the wind's timer runs; every peer must hold the host's fallen leaves at the
//             host's places and no leaf of its own. Red: leaves_lane_off.
//   leaves_join -- the late join: the host grows its fallen leaves, the first client holds them, leaves and joins again (its
//             second life run with leaves_rejoin) and must hold them at the host's places: `rig: VERDICT grime-leaves-join`.
//             Two peers. Red: leaves_lane_off.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::clean_drill {

// The dev lanes' tick. A latched read when the drill is off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::clean_drill
