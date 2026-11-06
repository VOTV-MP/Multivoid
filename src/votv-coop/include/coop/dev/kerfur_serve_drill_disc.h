// coop/dev/kerfur_serve_drill_disc.h -- leg D of the Omega serve drill: what a client's get_reports does when the client
// holds a disc (env VOTVCOOP_KERFUR_SERVE_DISC = on, swap, wh or busy, with the drill on; BOTH peers). It replaces legs A
// to C. The client takes a disc, aims at the Omega's mirror and presses get_reports as a player does; the seat sends it
// to the host with the disc's record, and the client counts what its own machine did with the host's answer. The host
// snapshots its side before, prints '[KERFUR-SERVE] leg D:' (consumed, consume-miss, hint, carries, host-published = the
// PropSpawn/PropDestroy its session accepted while the copy lived) and JUDGES by variant, with the field named on FAIL:
// 'rig: VERDICT kerfur-serve-disc PASS|FAIL|INVALID'. on: consumed once, the Omega holds it, nothing published. swap:
// one consume-miss. wh: disc kept, the game's hint, no copy. busy: as on, then a second press is refused, its disc kept.
// Not drivable, so not built: the serve fallback, and an Omega that already holds a disc (it is on its job after the
// first consume, so a second press meets the job gate first). A press the client's own copy ran is FAIL.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kerfur_serve_drill::disc {

// Whether leg D is on (the knob, beside the drill's own). Resolved once.
bool Enabled();

// This peer's leg D, every pump tick in a world. Host: a state machine on the game thread. Client: starts one
// worker thread, which waits on the game thread for what it reads. Game thread.
void Tick(coop::net::Session* s);

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::kerfur_serve_drill::disc
