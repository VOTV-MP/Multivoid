// coop/dev/join_prop_loss_drill.h -- [dev] the join prop-loss drill: measures the two ways a host's prop is lost to a
// joining client, on whatever bytes it runs. Inert unless the join_prop_loss_drill row is set; the arm is its value.
//   break     -- the client breaks a resting mirror of a host beer can with an impact no player caused; the host's own
//                can must survive it.
//   breakown  -- as break, with the client's own player as the cause; the break must still reach the host.
//   rekey     -- the client destroys one of two cans that stand 13.7 cm apart before the join's snapshot bracket;
//                after the bracket the other can must still carry its own key and each key must resolve to its own
//                actor, and the host must still hold both.
// The peers sequence each other with drill_chat lines; nothing crosses by file. Lines are tagged [JPL-DRILL]; each
// peer prints `<role> <arm> PASS|FAIL|INCONCLUSIVE` and its last line is `<role> DONE`.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::join_prop_loss_drill {

// Game thread, once per pump tick, after the world is up; a latched enum read when off.
void Tick(coop::net::Session* session);

// Game thread, every pump tick including the world load; a latched enum read when off. The rekey arm's client half
// runs here, since its destroy must come before the join's snapshot bracket.
void TickLoad(coop::net::Session* session);

// The cans, the pair and the steps belong to one session.
void OnDisconnect();

}  // namespace coop::dev::join_prop_loss_drill
