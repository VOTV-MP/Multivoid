// coop/props/prop_food_sync.h -- one author for a world food's temperature, ripeness and use count: the host.
//
// A food's tick rolls its own temperature and spoilage, and at the ends of the range spawns a burnt or frozen copy and
// destroys itself, so every peer running it is a separate author, and a second burnt food. A client's world food is
// therefore parked (its actor tick off) and takes the host's values, sent when they move. A client's bite runs on its
// copy, for its own hunger and effects, and goes to the host as an intent; the host's count is the answer. The last
// bite destroys the client's copy, and that destroy crosses on the prop lane.
//
// A food in a hand is the holder's: it keeps its tick there, and its condition goes to the others for the copy they show
// in that hand, parked as well, since a mirror's own tick could burn. When a client lets one go, the host's new copy
// has none of that condition, so the client names it once and the host takes over. Parked copies paint their own looks.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct FoodStatePayload;
struct FoodBitePayload;
struct FoodHandPayload;
}  // namespace coop::net

namespace coop::props::prop_food_sync {

void Install(coop::net::Session* session);

// Per gameplay tick: the host's poll or the client's parking, twice a second. Game thread.
void Tick();

// CLIENT: the host's condition of one food.
void OnState(const coop::net::FoodStatePayload& p);

// HOST: a client bit one food. Validated at the router.
void OnBite(int senderSlot, const coop::net::FoodBitePayload& p);

// HOST: a client let go of a food it held; this is the condition its hand kept. Validated at the router.
void OnRelease(int senderSlot, const coop::net::FoodStatePayload& p);

// ANY: the condition of the food in `senderSlot`'s hand. Validated at the router.
void OnHand(int senderSlot, const coop::net::FoodHandPayload& p);

// HOST: a joiner reached its ready edge. Every food goes out again, on the poll's own budget.
void QueueConnectBroadcastForSlot(int slot);

// Session end: a parked copy gets its tick back, since it has its own world again. Game thread.
void OnDisconnect();

}  // namespace coop::props::prop_food_sync
