// coop/world/power_upgrade.h -- the host's record that a client spent an upgrade at a generator, and the refund of
// an install the host refused after the item was spent. Principle-7 gameplay layer, beside coop/world/power_grid,
// whose install op takes the record. A client's insert (generator playerUsedOn @3343) destroys the held upgrade and
// raises its own copy's level before the op reaches the host, so two players installing at a generator's last free
// place leave the second one's upgrade spent and its op refused. The host hears every client destroy it applies
// (coop/props/remote_prop): an upgrade destroyed within reach of a generator's upgrade slot is a spend, recorded
// against its sender and that generator. An upgrade from the inventory is the hand's, and TakeHandSpend judges it. An install is taken only against a spend, and a refused one is refunded
// only against one, so a refund never exceeds an item paid.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::power_upgrade {

// Register the destroy listener and keep `session`. Idempotent. Game thread.
void Install(coop::net::Session* session);

// HOST: take the newest spend `slot` made at generator `index`, burning it. False when there is none. Game thread.
bool TakeSpend(uint8_t slot, int32_t index);

// HOST: an install paid by the upgrade in `slot`'s hand. An upgrade taken out of the inventory is the hold slot's own
// actor, the player's expression and never a world prop, so its destroy reaches no one; the hand lane's history
// (coop/player/hand_history) says the hand held one within a few seconds of the op's arrival `arrivedMs` (the
// net clock). One such spend a slot a second, since an honest player takes the next upgrade out before the next
// install. Game thread.
bool TakeHandSpend(uint8_t slot, uint64_t arrivedMs);

// HOST: one upgrade back to `slot`, spawned where its body stands (above `gen`'s upgrade slot when the body is
// unread), for an install refused after its spend; the host's spawn watcher gives it to every peer. Game thread.
void Refund(uint8_t slot, void* gen);

// Every record goes at a session's end; a slot's go with its occupant. Game thread.
void OnDisconnect();

// [dev] the grid drill's count on the host: the refunds spawned.
uint64_t RefundsSpawned();

}  // namespace coop::power_upgrade
