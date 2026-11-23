// coop/world/hole_sync.h -- the Hole Repair (rozitalHole) on every player's machine.
//
// The event spawns event_bottomHoleController_C on the host alone, a save actor whose box (by the main computer) opens
// the Hole: the bottom room (holeBottom_C), the device and its four beacons (props, on the prop lanes), the patrol
// controller, whose guards (kavotia_C, the NPC lane) and Igetis (igetis_C, the world-actor lane) cross as its spawns,
// inert on a client (coop/world/spawn_authority). A joiner loads the controller and the room from its save.
// HOST: its room's place every 5 s (EventCue 117), and that its controller stands (118). CLIENT: the controller's box
// refused; a room made at 117 where it has none; its own controller ended once 118 stops.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

// The guards. A guard's sense and its charge trace from its head to its target and give up on a hit, which a puppet's
// body is, so the host's guards did not take a client for their target. HOST: a guard that sensed nobody, while the
// Igetis is not visited, is handed the nearest client's body within 50 m, and a client's body within 5 m of the Igetis
// visits it, as the host's player does; a guard's trace while it targets a client's body reads clear (it may fire
// through cover at a client). Each shot goes out (119 its rotation, 120 its place), and one whose line (500 m along its
// forward) crosses a client's body (70 cm) is that client's addDamage (coop/player/player_hit); a wall between is not
// seen. The Igetis's montage start goes out (121).
// CLIENT: a shot's copy fires for its beam, its point damage a thousandth; a Rozital shot's hit on this player is
// refused, since the host's is delivered; the Igetis's copy plays the montage at 121. Game thread.

namespace coop::hole_sync {

// The EventCue id; the cue registry's ids are append-only.
inline constexpr uint32_t kCueHoleRoom = 117;   // the bottom room's place
inline constexpr uint32_t kCueHoleLive = 118;   // the host's controller stands
inline constexpr uint32_t kCueShotRot = 119;    // a guard's shot: x, y, z = its pitch, yaw, roll
inline constexpr uint32_t kCueShotAt = 120;     // a guard's shot: its place, after its rotation
inline constexpr uint32_t kCueIgetisPlay = 121; // the Igetis's montage starts (its place)

void Install(coop::net::Session* session);

// The watch, retried until registered; the host's room heartbeat. Game thread, once a tick.
void Tick();

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

void OnDisconnect();

}  // namespace coop::hole_sync
