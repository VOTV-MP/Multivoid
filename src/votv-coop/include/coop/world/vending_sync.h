// coop/world/vending_sync.h -- the snack and soda vending machines (prop_vending_C), the host's.
//
// A machine's stock is its own arrays (`retarget_items`, `retarget_prices`, `retarget_amounts`), its `balance` (the coins put in) and
// `isEmpty`; its use (a buy, taking the change, a coin put in) runs on the peer that used it, and the item it mints is a prop that
// peer alone holds (a client authors no prop spawn); the refill is `beginRestock`, which waits for no line of sight between the
// machine's figure and the camera of the machine's own peer. Each peer's machine was its own. Now:
//   - HOST: once a second, each machine's state is sent, one AmbientFixtureState (family kVendStock) per stock slot: `id` is a hash
//     of the machine's interactable key, `x` the slot's amount, `y` its price, `z` the balance, `pitch` the slot count, `yaw` isEmpty,
//     `_pad[0]` the slot, `birthKey` the item's name; every machine is sent again to a joiner.
//   - CLIENT: its copy's amounts, prices, balance and isEmpty are written from them and its name boxes refreshed (gennameboxes); a
//     use is refused on the copy (actionOptionIndex, playerUsedOn) and asked of the host on PropIgniteIntent (verbs kVerbVendUse and
//     kVerbVendCoin), whose own run mints the item as a host prop; the refill (beginRestock, checkStock, timerCheckStock) is
//     refused on a client. The copy plays the machine's own figure sound (`audio_figura`) when the host's refill reaches it.
// What stays per peer: the figure's line-of-sight wait reads the host's camera alone; the figure's sound is the only presentation of it.
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AmbientFixtureStatePayload;
}  // namespace coop::net

namespace coop::vending_sync {

// PropIgniteIntent's verbs: a buy or a taking of the change (the action in pad byte 0, the stock slot in pad byte 1, the machine's
// key in `key`), and a coin put in (the machine's key in `key`, the coin's element id in `elementId`).
inline constexpr uint8_t kVerbVendUse = 10;
inline constexpr uint8_t kVerbVendCoin = 11;

void Install(coop::net::Session* session);
void Tick();
void OnState(const coop::net::AmbientFixtureStatePayload& p);
// HOST: a client's PropIgniteIntent of verb 10 or 11. False when the verb is not this lane's.
bool TakeIntent(int senderSlot, const uint8_t* bytes, uint16_t len);
void QueueConnectBroadcastForSlot(int slot);
void OnDisconnect();

}  // namespace coop::vending_sync
