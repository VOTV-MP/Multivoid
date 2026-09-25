// coop/dev/kerfur_convert_drill.h -- drill: a client turns a kerfur off and back on, the way its radial
// menu and its E-press would, while one disc of the client's own lies beside it (ini
// kerfur_convert_drill=1 / env VOTVCOOP_KERFUR_CONVERT_DRILL; BOTH peers).
//
// The host, from its arm on, keeps every kerfur NPC it has carrying a disc (type 0, red), and once a
// client's join is over spawns one more, so whichever kerfur the client turns off carries one: element
// ids are recycled, and the host's spawn can reach the client before the client's load tail is quiet, so
// no id and no before-and-after count tells the client which kerfur is the new one. The client, once its
// load tail has quiesced, takes the first kerfur NPC mirror it has, spawns a green disc of its own 2 m
// from it -- a local actor no lane expresses, the profile F-15 destroyed -- and calls its dropKerfurProp,
// then, once the prop form is its mirror, the prop's actionOptionIndex with the turn-on action. Each
// step waits on the object it needs; a step that cannot run ends the drill INVALID. The client's DONE
// line gives the verdict: both conversions applied; each form still standing when the client's own
// call returned, since the host converts and a client that converted locally destroyed it inside the
// call; the carried disc arrived once; and the green disc still alive one observation window (6 s,
// past the 4 s the old ghost custody waited) after the turn-off was applied.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kerfur_convert_drill {

// Cache the session, for the role. Called from the subsystems Install fanout every pump tick; idempotent.
// No-op when off.
void Install(coop::net::Session* session);

// Advance this peer's phase. Every pump tick in a world. Game thread.
void Tick();

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::kerfur_convert_drill
