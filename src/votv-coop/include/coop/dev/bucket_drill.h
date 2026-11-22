// coop/dev/bucket_drill.h -- drill: a water bucket's, a concrete bucket's and a cement bag's state is the host's (ini bucket_drill=
// off|soap|dip|mix|trowel|transfer|pin|visuals|ctick|drop|join|mop|alc|brew / env VOTVCOOP_BUCKET_DRILL; BOTH peers; the reds are the
// values of bucket_red). Each arm ends on one `rig: VERDICT bucket_<arm> PASS|FAIL|INVALID` line, said by the host
// (coop/session/rig_verdict.h); a FAIL is said where it is decided and nothing after it runs, a bound only says INVALID. The host fills
// buckets on the water, carries each and sets it down before the client's puppet by the game's own place; the client holds an item and
// runs the verb as a player does; the peers pace each other with drill_chat lines.
//   soap, dip, mix, trowel, transfer   the client's verb is asked of the host, applied there, and the row past the commit reaches the
//                                      client's copy; the host's target moved by the BP's own delta, and the client's held side too.
//   pin       a client copy dipped by our own call is put back to the host's water at its next tick.
//   visuals   a client copy's drawn water reaches the host's after a dip.
//   ctick     a client's concrete bucket copy never runs its own tick.
//   drop      a client's held bucket reaches the host in its drop's record; a forged record is refused.
//   join      before the join the host soaps a bucket and dries a concrete bucket; the joiner's copies match after the host's join send.
//   mop       the host's dip of the mop bucket reaches the client's copy of it.
//   alc       a client copy's deleteAlc is refused. brew (its own run, ~10 min): the host's brew reaches the client, a jar is filled.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::bucket_drill {

// Advance this peer's steps. Every pump tick in a world. Game thread. A latched enum read when off.
void Tick(coop::net::Session* session);

// The session ended: back to the first step.
void OnDisconnect();

}  // namespace coop::dev::bucket_drill
