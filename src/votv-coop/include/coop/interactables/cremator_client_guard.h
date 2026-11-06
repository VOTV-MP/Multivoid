// coop/interactables/cremator_client_guard.h -- a client's cremator copy acts on nothing.
//
// The cremator is the host's machine (cremator_state, cremator_intent). A client's copy still runs the Blueprint's
// own graph, a second machine beside the host's, so four gates hold it to its display, on a client only:
//   - `addObject` is refused: the copy's `actors`, `health` and `giveFuel` stay empty, so nothing ignites or burns.
//   - the door's `open` is refused while the copy is active or holds an ON want; the lane's own apply and a call
//     before the client's world is announced pass.
//   - a valve press is refused and not sent (the pour runs from `gasAmount`, which does not cross, docs/scope.md);
//     the player is shown the game's own hint, once a burst. The handle's press is cremator_intent's.
//   - `spitFireballs_timer` is refused: the copy shows no blow-out fireballs. The host's are not synced either; they
//     are a host-only effect (docs/scope.md).
// Dev red arm: cremator_client_guard_off. MTA: a client's use of a shared element is a request the server runs
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3018, Packet_Vehicle_InOut). Divergence: the valve press
// and the door open are refused on the client without a request, since the host's lid lock (prop_swinger::open) and
// the unsynced fuel already decide them; MTA asks the server (CGame.cpp:3018) for the shapes that cross.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::cremator_client_guard {

// Registers the gate's four name watches, once per session start (coop/interactables/verb_lanes). A watch the gate
// refused is final for the process and said once. Game thread.
void Install(coop::net::Session* session);

// Settles the watches and says once when all are live. Game thread, once per pump tick.
void Tick();

// The session ended: the counters, with one summary line.
void OnDisconnect();

// How many `addObject` calls this client has refused since the session began; the guard drill's verdict reads it.
// Game thread.
uint64_t AddObjectRefused();

}  // namespace coop::cremator_client_guard
