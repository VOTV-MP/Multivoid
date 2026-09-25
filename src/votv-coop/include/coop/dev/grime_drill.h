// coop/dev/grime_drill.h -- [dev] the grime lane at its verb: a clean on either peer reaches the other's
// copy, a partial wipe as its fall and a wipe to destruction as zero (coop/interactables/grime_sync).
//   BOTH PEERS -- pick the same two decals (the two lowest position keys among the cleanable grime_C decals
//                 whose process is above 20: one save, one set), the client the first and the host the
//                 second, and run the same legs on their own one, the rain's own call form,
//                 clean(nullptr, Sub, noSound): PARTIAL (Sub 10) and, once this peer's copy of the other
//                 peer's decal has fallen, DESTROY (Sub 1000, the process below zero). Each ends when its
//                 copy of the other's decal reads zero and is still alive -- an apply repaints, it never
//                 destroys. A phase that does not land in 15 s fails.
// Lines are tagged [GRIME-DRILL]; run with grime_drill=1 on both peers; the client's DONE line ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::grime_drill {

// Game thread, once per pump tick; a single bool read when off; two decal reads a tick while on.
void Tick(coop::net::Session* session);

// The decals and the phase belong to one world and one session.
void OnDisconnect();

}  // namespace coop::dev::grime_drill
