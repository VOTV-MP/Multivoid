// coop/world/power_grid_install.h -- the hand's half of a generator upgrade install, beside coop/world/power_grid: what the
// upgrade in a client's hold slot is to the host's judge (paid by hand as the request arrives) and to the client (the
// hold announced before the insert spends the upgrade). Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::power_grid::install {

// HOST: whether the upgrade in `sender`'s hand paid for the install `p`, judged as the request arrives: the hold it was
// sent from, if the hand lane still owes it, is applied first, then hand_spend counts the use. The result goes with the
// queue entry, so a request that waits in the queue behind an earlier one is not judged against a hand that has moved on.
// A request Spend never counted is the client's loss: the host cannot see its inventory, so it refunds only what it
// counted. An honest client reaches that only by a same-tick swap of two identical items (no new hold generation) or by
// outrunning the hand lane's budget and its debt (ApplyOwedNow).
bool PaidByHand(coop::net::Session& session, uint8_t sender, const coop::net::PowerGridPayload& p);

// CLIENT: the body of generator_C::playerUsedOn is about to run with `locals` (its frame), and spends the upgrade it is
// handed. An upgrade from the hold slot is announced to the host first, so the hold is on the one ordered lane ahead of
// the install op, even when it was picked up and used within one tick. A hold already announced makes none.
void AnnounceHeldUpgrade(coop::net::Session& session, void* function, const uint8_t* locals);

}  // namespace coop::power_grid::install
