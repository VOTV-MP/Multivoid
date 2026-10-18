// coop/props/prop_hold.h -- who holds or has reserved a world prop, read on the host by the take-or-use judge
// and the remover's judge. Gameplay layer (principle 7).
//
// "Held by someone else" is one predicate: another slot's drive on the prop, another player's hook pulling it, the
// host's own grab or hold, or another slot's reservation. A reservation is the host's promise to one client that the
// prop is its to act on until it commits or aborts; the table lives here so that the release is the only way out.

#pragma once

#include <cstdint>

namespace coop::prop_hold {

// The host player's own hand holds `actor`: its grab or its hold.
bool HostHolds(void* actor);

// A peer other than `requester` holds or reserves `actor`: another slot's drive, another player's hook pulling it
// (the requester's own hook never counts, as its own drag does not; a hook anchored to the world or placed by the
// level is no player's hold), the host's own hold or last-held actor (when `requester` is not 0), or another slot's
// reservation. A world prop is never in a peer's hand (the hand holds a
// hand view), so there is no peer-hand term.
bool HeldByOther(void* actor, int requester);   // WhoHolds(...) != Holder::None

// Who holds `actor` against `requester`: the first found, in this order. HolderWhy names it for a refusal's tally.
enum class Holder : uint8_t { None, Drive, Hook, HostHand, Reservation };
Holder WhoHolds(void* actor, int requester);
const char* HolderWhy(Holder h);   // "" "held by a peer's drive" "pulled by a hook" "held by the host's own grab"
                                   // "reserved by another peer's take"

// A slot other than `requester` holds a reservation on `actor` (0 = the host asks).
bool ReservedByOther(void* actor, int requester);

// The reservation table: at most one per slot, cleared only by Release. False when the slot already holds one or
// is outside 1..kMaxPeers-1.
bool Reserve(int slot, uint16_t id, uint32_t eid, void* actor);
bool HoldsReservation(int slot);
// The reservation matching (slot, id, eid): true, and `*outActor` = its CachedObjRef's Get() (null when the actor
// died); false when no reservation of that slot matches.
bool Match(int slot, uint16_t id, uint32_t eid, void** outActor);
void Release(int slot);
int ReservationCount(int slot);  // 0 or 1
uint32_t ReservedEid(int slot);  // the reservation's eid, 0 when none
// Every function treats a slot outside 1..kMaxPeers-1 as holding nothing (false, 0, a no-op).

}  // namespace coop::prop_hold
