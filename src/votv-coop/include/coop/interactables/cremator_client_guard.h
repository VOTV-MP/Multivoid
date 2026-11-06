// coop/interactables/cremator_client_guard.h -- a client's cremator copy acts on nothing.
//
// The cremator is the host's machine (cremator_state, cremator_intent). A client's copy still runs the Blueprint's own
// graph: its begin-overlap collects whoever stands in the firebox (`addObject`), its tick burns them, a player's use
// can open its door, and a press turns its valve. Each is a second machine beside the host's, so three gates hold the
// copy to its display (windup ramp, light, burn audio, door latch), on a client and never on the host:
//   - `addObject` is refused: the copy's `actors`, `health` and `giveFuel` stay empty, so nothing in it ignites, burns
//     or destroys a prop. The refusal holds before the world is announced too: a client's copy collects nothing, ever.
//   - the cremator door's `open` is refused while the copy is active or the lane holds an ON want for it, since a copy
//     is unlocked for the trip the host's state takes and the host's own door is locked; the lane's own apply and a
//     call made before the client's world is announced (the host's save loading) pass.
//   - a press of the fuel valve is refused and not sent: the pour it would start runs from `gasAmount`, which does not
//     cross, so a client's copy holds no fuel to show (docs/scope.md). The handle's press is cremator_intent's.
// Dev red arm: cremator_client_guard_off. MTA: a client's use of a shared element is a request the server runs
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3018, Packet_Vehicle_InOut).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::cremator_client_guard {

// Registers the gate's three name watches, once per session start (coop/interactables/verb_lanes). A watch the gate
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
