// ue_wrap/devices/atv_passenger.h -- engine access for a rider on the ATV's passenger seats: where each seat is, a body
// attached to it, and the spot beside the ATV a leaving body is put at. Principle-7 engine-wrapper layer (NO network or
// coop state); coop::atv_passenger decides who sits where. The ATV's own seat is the game's (it possesses the ATV); a
// passenger is a Character that is NOT possessing it, carried by an attachment to the rig's root body, so the engine moves
// it with the rig each frame and nothing trails.
//
// Geometry: the ATV's root Mesh is the actor root, so a seat is an offset in the root's local space (X forward, Y right,
// Z up, cm) of the rider's ACTOR -- the capsule centre, the position a pose stream carries. Every machine places a rider
// from the same constants, so a puppet sits where the player's own body does.

#pragma once

#include "ue_wrap/core/types.h"  // FVector, FRotator

#include <cstdint>

namespace ue_wrap::atv_passenger {

// The passenger seats of one ATV (protocol.h: kAtvPassengerSeats).
inline constexpr int kSeats = 3;

// Seat `seat`'s offset in the ATV root's local space: the rider's actor, capsule centre. (0, 0, 0) for a seat out of range.
FVector SeatOffset(int seat);

// The world position of seat `seat` of `atv`: the root body's transform applied to the offset. False when the ATV's root
// transform does not read or the seat is out of range; `out` is then untouched. Game thread.
bool SeatWorldLocation(void* atv, int seat, FVector& out);

// Seat `rider` (a mainPlayer_C: the local player or a puppet) on seat `seat` of `atv`: its collision off, so it cannot push
// the rig, and attached to the rig's root body at the seat, its facing kept. A local player (`local`) also has its
// movement mode set to None, so no input moves it; its controller and camera stay its own. False, with nothing left
// half-done, when the ATV or the rider is not live or the attach did not dispatch. Game thread.
bool SeatRider(void* rider, void* atv, int seat, bool local);

// Move an already seated `rider` to seat `seat` (the host moved it to another seat). False as SeatRider.
bool MoveRider(void* rider, void* atv, int seat);

// Take `rider` off the rig: detached, then put at `placeAt` (a capsule centre; its own yaw kept) when one is given, then
// collision on, and for a local player its movement mode back to Walking. The place comes before the collision, so the
// move cannot be refused for the body overlapping what it is put beside. Called for a rider whose ATV may be gone.
// False when the rider is not live. Game thread.
bool ReleaseRider(void* rider, bool local, const FVector* placeAt = nullptr);

// Whether `rider`'s root is attached to `atv`. False for either null, or a call that did not dispatch. Game thread.
bool IsRiderOf(void* rider, void* atv);

// The spot a rider leaving `atv` stands at, a capsule-centre position: beside the ATV, off the side of the seat `seat`
// first, then the other side and the rear. Each candidate is clear of a wall between it and the rig and has ground under
// it, found by a trace down from above it; none found falls back to the NavMesh point nearest the rig. `halfHeight` is the
// rider's capsule half height. False when no spot was found; `out` is then untouched. Game thread.
bool FindExitSpot(void* atv, int seat, float halfHeight, FVector& out);

// The same for a body whose ATV is gone: the ground under `from` (a capsule centre), or false when none is found.
bool FindGroundBelow(void* worldCtx, const FVector& from, float halfHeight, FVector& out);

// Whether `player` may board: its hands are empty (the hand and the grav hand, as the sit verb's gate
// reads them) and its fall speed is under the sit verb's limit. `why` (optional) names the gate that refused. Game thread.
bool MayBoard(void* player, const char** why = nullptr);

}  // namespace ue_wrap::atv_passenger
