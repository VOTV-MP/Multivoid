// coop/dev/kerfus_drill.h -- drill: a client turns the plain kerfur (the Kerfus) on through the host,
// and it follows that client, not the host (ini kerfus_drill=1 / env VOTVCOOP_KERFUS_DRILL; BOTH peers).
//
// The host stays where the save put it. The client, once its load tail has quiesced, walks to the save's
// Kerfus with the director (the host judges a press by the sender's reach) and presses it on the way its
// E-press would (actionOptionIndex, action 8): its own copy must stay off until the host's state turns it
// on, else FAIL at once. The client then walks to a pile 10 to 30 m away that the director's pick keeps 10
// m from the host, and the Kerfus must settle within 2 m of the client for 5 s, inside 45 s of its
// arrival: one that settles by the host is the defect (FAIL), by neither is INVALID. Then the host
// possesses it -- the haunting's own verb -- toward a pile 10 to 30 m from the client. The client sees
// that as the haunting's energy pin and walks back to where it pressed the Kerfus (possessed 1 m from its
// player, a Kerfus stalls), and it must go off at least 6 m from where it settled, its copy having moved
// on the host's drive stream (PASS); one that follows the client back, on, is FAIL. Every wait has its
// readiness or a window; a step that cannot run ends the drill INVALID. The rig client must join at the
// base (tools/rig_profile_pose.py): the save's Kerfus is there.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kerfus_drill {

// Cache the session, for the role. Called every pump tick from the dev wiring; idempotent. No-op when
// off.
void Install(coop::net::Session* session);

// Advance this peer's phase. Every pump tick in a world. Game thread.
void Tick();

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::kerfus_drill
