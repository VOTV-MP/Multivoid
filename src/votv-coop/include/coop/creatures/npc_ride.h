// coop/creatures/npc_ride.h -- the one owner of a kerfur mirror riding an ATV on a client.
//
// The host's ATV stamp names the kerfur that rides it (riderEid, coop/interactables/atv_seat_table). This owner keeps that
// as a row per kerfur eid and acts on it at the events that can change the answer: the stamp, the mirror's bind (an
// adopted twin or a fresh spawn) and its reveal after the join window. With a live ATV and a ready mirror it seats the
// mirror through the kerfur's own sitOnCar, which attaches it, turns its collision off and names it the ATV's rider; when
// the stamp ends the ride it unseats the mirror without a car (ue_wrap::kerfur::UnseatWithoutCar), because a mirror's pose,
// state and movement are the host's stream's. While it holds the mirror the pose stream writes only its head and state.
// The row lives here, not on the Npc, so a stamp that arrives before the mirror's bind is applied by the bind or the reveal.
// A client only; the host's own kerfurs ride by the game. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::npc_ride {

// The live ATV filed under `key` (null when there is none) and its object index.
using Resolver = void* (*)(const std::wstring& key, int32_t& atvIdx);
void SetResolver(Resolver fn);

// What the owner did for an eid, told to the observer after each outcome: the mirror taken off its ride, riding, or held
// (no mirror bound yet, the mirror hidden, no ATV to sit on or a sit that did not attach, a class that is no kerfur).
// `sitarrowCm` is the kerfur's distance to the ATV's `sitarrow` at a ride; `wasAttached` at an off says the mirror was still
// attached to an ATV when the clear began. Null = no call and none of the reads. Game thread.
enum Outcome : int { kOff = 0, kRides = 1, kHeldUnbound = 2, kHeldHidden = 3, kHeldNoAtv = 4, kRefusedClass = 5 };
using Observer = void (*)(uint32_t eid, int outcome, const std::wstring& key, float sitarrowCm, bool wasAttached);
void SetObserver(Observer fn);

// The kerfur `eid` rides the ATV under `key`. Level-wise: the same answer again is cheap and safe.
void Set(uint32_t eid, const std::wstring& key);

// The kerfur `eid` no longer rides the ATV under `key`: the row goes only when it names that key, and the mirror is
// unseated.
void Clear(uint32_t eid, const std::wstring& key);

// The kerfur mirror of `eid` was just bound: a new actor is not attached, so a stored row is applied to it.
void OnMirrorBound(uint32_t eid);

// The mirror of `eid` was just revealed by mirror_defer: a row held while it was hidden is applied.
void OnRevealed(uint32_t eid);

// True while the mirror of `eid` is seated by this owner on a live ATV: its pose is the ATV's and the stream's location,
// rotation and movement are not written. A memory test, no engine call.
bool HoldsPose(uint32_t eid);

// True while a row for `eid` is stored, whether or not it holds the pose (the riders drill reads it after a Clear).
bool HasRow(uint32_t eid);

// The session ended: every row is dropped; the mirrors are destroyed by the teardown. Game thread.
void OnDisconnect();

}  // namespace coop::npc_ride
