// coop/dev/kerfur_serve_drill.h -- drill: the host runs a client's commands to the kerfur Omega for that
// client (ini kerfur_serve_drill=1 / env VOTVCOOP_KERFUR_SERVE_DRILL; BOTH peers).
//
// Once the client's join is over the host spawns an Omega in front of itself, takes a red disc in hand by
// the game's own pickup and stays put. The client, its load tail quiet, the Omega's mirror come and the
// host's hand item a red disc, commands get_reports first, as its radial menu would. Leg B, on the host as
// that verb returns (the disc insert runs inside it): the host's disc is still in its hand, the Omega holds
// none and is back in the follow state. The client then walks with the director to a pile 10 to 30 m away
// and at least 10 m from the host, and commands follow. Leg A, judged on the client from its own command,
// the same moment on every build: its mirror settles within 2 m of it for 5 s in the follow state, inside
// 60 s. Then it commands idle, and the host runs the kill verb itself. Leg C: as that returns the Omega is
// murderous and its move() went for the host, not the client it served. '[KERFUR-SERVE] host DONE' ends it,
// legs B and C in it and leg A in the client's line; '[KERFUR-SERVE] INVALID' is a step that could not run.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kerfur_serve_drill {

// Cache the session, for the role. Called every pump tick from the dev wiring; idempotent. No-op when
// off.
void Install(coop::net::Session* session);

// Advance this peer's phase. Every pump tick in a world. Game thread.
void Tick();

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::kerfur_serve_drill
