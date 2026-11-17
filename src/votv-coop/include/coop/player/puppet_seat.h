// coop/player/puppet_seat.h -- the one owner of a remote driver's body sitting in its ATV's seat.
//
// The seat table (coop/interactables/atv_seat_table) says which ATV a peer sits on; this owner keeps that as a target per
// peer slot and acts on it at the events that can change the answer: the seat change, the body's birth, a ragdoll stop.
// With a live ATV and a body it attaches the body to the ATV once (K2_AttachToActor, KeepWorld), so the engine carries the
// body with the ATV and nothing writes it per frame, sets the AnimBP's seat pose (`isOnAtv`) and turns collision off. When
// the target goes it detaches, restores the pose and collision, and tells the RemotePlayer to re-base its yaw.
// The target lives here, not on the RemotePlayer, so a seat stored before the peer's first pose is applied by the birth call.
// Every entry point returns at once for a slot at or past kMaxPeers. Game thread.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::puppet_seat {

// Resolves seat `seatIndex` of the ATV filed under `key`: the live ATV, its object index and the seat's world transform.
// False when there is no live ATV or no seat transform (atv_seat_table::ResolveSeat).
using Resolver = bool (*)(const std::wstring& key, uint8_t seatIndex, void*& atv, int32_t& atvIdx, ue_wrap::FVector& loc,
                          ue_wrap::FRotator& rot);
void SetResolver(Resolver fn);

// What the owner did for a slot, told to the observer after each outcome: released, attached, held (a target and no body yet)
// or failed (no live ATV, no seat transform, or an attach that did not run). `offsetCm` is the body's distance from the ATV's
// root at an attach, `onAtv` its AnimBP's seat pose and `collision` its collision, read after the outcome's own writes.
enum Outcome : int { kReleased = 0, kAttached = 1, kHeldNoBody = 2, kResolveFailed = 3 };
using Observer = void (*)(uint8_t slot, int outcome, const std::wstring& key, float offsetCm, bool onAtv, bool collision);
// Null = no call, and none of the reads. Game thread.
void SetObserver(Observer fn);

// The slot's peer sits in seat `seatIndex` of the ATV under `key`. Level-wise: the same answer again is safe (the attach is
// skipped; the seat is resolved and the pose and collision written again, about six engine calls).
void Set(uint8_t slot, const std::wstring& key, uint8_t seatIndex);

// The slot's peer left the ATV under `key`. Clears the target only when it names that key, so a stale record of another
// ATV never clears a newer one.
void Clear(uint8_t slot, const std::wstring& key);

// The slot's body was just spawned: it starts from the engine's own state, and a stored target is applied now.
void OnPuppetSpawned(uint8_t slot);

// The slot's body came back from a ragdoll display (the attachment the display made is gone): a stored target is applied.
void OnBodyChanged(uint8_t slot);

// True while the slot's body is attached to a live ATV by this owner: its pose is the ATV's, and the stream's pose is not
// written. A memory test, no engine call.
bool HoldsBody(uint8_t slot);

// The slot's occupant left: its target and latches are dropped. The body is destroyed by the caller.
void Forget(uint8_t slot);

// The session ended: every slot is dropped, the bodies are destroyed by the teardown.
void OnDisconnect();

}  // namespace coop::puppet_seat
