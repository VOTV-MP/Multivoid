// coop/dev/kerfur_serve_drill.h -- drill: the host runs a client's commands to the kerfur Omega for that
// client (ini kerfur_serve_drill=1 / env VOTVCOOP_KERFUR_SERVE_DRILL; BOTH peers).
//
// The host spawns an Omega in front of itself once the client's join is over, takes a red disc in hand and
// stays put. The client walks to it with the bot director and commands as a player does, through the Omega's
// radial, within use reach (it walks up to the mirror, aims, presses): get_reports, then follow, then it walks
// to a pile 10 to 30 m away. Leg B, on the host as get_reports returns: its disc is still in its hand, the Omega
// holds none and is back in the follow state. Leg A, on the client: its mirror settles within 2 m of it for 5 s
// in the follow state, inside 60 s of its reaching the pile. It then walks back up and commands idle, and the
// host runs the kill verb itself. Leg C: as that returns the Omega is murderous and its move() went for the host,
// not the client it served. One line (coop/session/rig_verdict.h), tag kerfur-serve: the host's PASS holds legs B
// and C, a leg that failed or a step that could not run is said by the peer that saw it.
// With kerfur_serve_disc=on (or swap, wh, busy) leg D (kerfur_serve_drill_disc.h) runs instead of legs A to C.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kerfur_serve_drill {

// Cache the session, for the role. Called every pump tick from the dev wiring; idempotent. No-op when
// off.
void Install(coop::net::Session* session);

// Advance this peer's phase. Every pump tick in a world. Game thread. The client starts one worker thread,
// which waits on the game thread for what it reads.
void Tick();

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::kerfur_serve_drill
