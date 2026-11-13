// coop/player/hand_spend.h -- an item in a client's hand, spent by an act the host judges.
//
// A verb that consumes the held item (a toolbox use, an ATV module, a tower fuse, a generator upgrade) runs on its
// client first and sends its request after; the host cannot see the client's inventory, so the evidence that the act
// was paid is the hand lane's own record of that slot (coop/player/hand_item): its last held class, and the hold
// generation that record numbers. The request itself is the spend, judged at the lane's take, as MTA's server judges
// a client's shot against the weapon it last saw (CBulletsyncPacket.cpp:193-201).
//
// A hold announced after the last spend starts over; a request with no hold behind it finds none left. The state is
// per slot and clears with the slot's occupant. Game thread, host.

#pragma once

#include <cstdint>

namespace coop::hand_spend {

// Whether `slot`'s last held class is `cls` and its current hold generation (hand_item::HoldGeneration) has a use left of
// `uses`: if so one use is counted and true is returned. The number moves on each hold the client's own hand lane
// announces, so it is a rate bound on that announce (kHandBudget), not the identity of an item: a client that empties
// its hand and announces a hold again starts a new one. The item is spent on its copy by then, so a use is counted
// whatever the verdict after it.
bool Spend(uint8_t slot, const wchar_t* cls, uint32_t uses);

}  // namespace coop::hand_spend
