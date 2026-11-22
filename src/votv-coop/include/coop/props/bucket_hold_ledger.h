// coop/props/bucket_hold_ledger.h -- the numbers of a client's held cement bag and held concrete bucket, as the host counts them.
//
// A held item is its holder's own and the host reads none of its numbers: the hand lane carries the class alone. A client's mix
// spends units of the held bag and makes concrete of them, and its transfer moves them, so the host keeps the count itself and
// the judges refuse what the count cannot pay. A concrete bucket's units cannot rise while it is held, so the units it had at the
// take bound the record of its drop. A trowel has no ledger: its take costs the host's own bucket a unit, so a forged take makes
// nothing, and its load is spent on cracks the host does not count. The count starts at the take: a client's granted pickup of a
// world bag or concrete bucket destroys the host's copy (remote_prop's destroy listeners). Its units seed the hold that follows, in
// either order: kept pending until the hold generation moves to a hold of that class, or taken at once by a hold of that class with
// no count yet, and never on a destroy alone, so a client that destroys a fuller bag while it holds a near-empty one changes
// nothing. A bag the host never saw taken from the world seeds from the class default, said once. The
// ledger belongs to one hold: the slot's next hold generation (coop/player/hand_item), the peer's leaving or the session's end
// empties it. Deliberate divergence from MTA, which checks a client's reported ammo against a weapon table the server owns
// (CPlayerPuresyncPacket.cpp:214-274): the host has no table of a client's hand, so its own count of what the verbs spent is the
// evidence, as coop/player/hand_spend keeps its use count. Game thread, host.

#pragma once

#include <cstdint>

namespace coop::bucket_hold {

// The class default of a cement bag's units (prop_cementBag.cpp `units = 50`).
inline constexpr int32_t kBagUnitsDefault = 50;

// Register the take edge: the destroy listener that keeps a take's numbers pending. Idempotent.
void Install();

// The units `slot`'s held bag holds by its ledger, and the ledger set to `units`.
int32_t BagUnits(uint8_t slot);
void SetBagUnits(uint8_t slot, int32_t units);

// The units the concrete bucket `slot` took from the world had at the take, or -1 when the host never saw one taken.
int32_t ConcreteUnits(uint8_t slot);

// Session end. A slot's own state is empty with its occupant (coop/player/roster_ledger).
void OnDisconnect();

}  // namespace coop::bucket_hold
