// coop/dev/kerfus_drill.h -- drill: a client turns the plain kerfur (the Kerfus) on and off through the
// host, and sees it follow the host (ini kerfus_drill=1 / env VOTVCOOP_KERFUS_DRILL; BOTH peers).
//
// The client, once its load tail has quiesced, takes the first Kerfus mirror it has, walks to it with
// the director (the host judges a press by the sender's reach), and presses it on the way its E-press
// would (actionOptionIndex, action 8). The press must leave the Kerfus off here -- a client never runs
// the Kerfus's verbs or brain -- until the host's state turns it on; a copy the press turned on here is
// the defect, and ends the drill FAIL at once. The host, once its Kerfus is on, walks to a nav-reachable
// pile 10 to 30 m away; the Kerfus follows the host, and the client's copy must move 5 m, held by the
// host's drive stream. The client then walks back to it and presses it off. Each step waits on the
// object it needs; a step that cannot run ends the drill INVALID. The client's DONE line gives the
// verdict: the press stood, the host turned it on, it moved on the host's drive stream, the host turned
// it off. The rig client must join at the base (tools/rig_profile_pose.py): the save's Kerfus is there.

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
