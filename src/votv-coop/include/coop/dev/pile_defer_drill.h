// coop/dev/pile_defer_drill.h -- [dev] the pile deferral drill: a joiner's real deferred pile binds, and what became
// of them (ini pile_defer_drill=check|giveup / env VOTVCOOP_PILE_DEFER_DRILL; BOTH peers, the same value). Run it on
// a fresh host world that rehosts, with one client. Lines are tagged [PILE-DEFER].
//
// The deferral is natural: a pile whose native had not loaded when its row arrived is marked deferred in bind_wait
// (pile_spawn_bind) and the quiescence sweep binds it later. The CLIENT counts those piles one by one, reads
// bind_wait's PropConvert tallies and, 20 s after its join is idle and once the load tail has quiesced, sends the host
// the pose of every trash element it has bound (DevParityReport). It prints one FAIL line per failed check, then its
// DONE line with the counts. The HOST compares each reported pose with its own pile of that eid (a pile at rest
// within 2 cm and 2 deg; a moving one is listed, not judged), names any pile the joiner lacks, and prints PASS or
// FAIL lines, then its DONE line, once per report (once per join).
// Arm giveup (with pile_late_load=1, the late-load seam) destroys the kept-aside clump once its row defers, to stand
// in for a native that never loaded; the sweep must give up on it, its held converts must replay unbound, and the pile
// must stand at the host's pose.
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::pile_defer_drill {

// Once per pump tick; a latched enum read when off. The host judges the reports that arrived, the client runs its
// window.
void Tick(coop::net::Session* session);

// The session ended: a rejoin's client counts and reports again.
void OnDisconnect();

// CLIENT, pile_spawn_bind: the bind of `eid` was deferred to the sweep (its native has not loaded).
void NoteDeferred(uint32_t eid);

// CLIENT, pile_spawn_bind: a save-loaded native was adopted as the mirror of `eid`.
void NoteBound(uint32_t eid);

// CLIENT, quiescence_drain: the sweep gave up on `eid` with no native.
void NoteGivenUp(uint32_t eid);

// CLIENT, trash_mirror: a mirror was stood up for `eid` (a spawn, or a form change); counted for a deferred eid.
void NoteMaterialised(uint32_t eid);

// CLIENT, remote_prop_convert: a convert met a deferred eid with no actor; `held` says bind_wait took it.
void NoteConvertAtDeferred(bool held);

}  // namespace coop::dev::pile_defer_drill
