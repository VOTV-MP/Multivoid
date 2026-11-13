// coop/props/prop_nametag.h -- a prop's nametag is the host's, on every peer.
//
// The nametag item writes its tag into the prop the player's left click hits and builds the prop's floating label
// (ue_wrap/actors/nametag). That ran wherever the click was and nowhere else: a client's tag stayed on its own copy,
// and a tag the host put on reached no client. Now, on the PropNametag kind:
//   HOST   -- its player's tag runs the game's own; the watch on the item's exit tells every client (op 2).
//   CLIENT -- its player's click is refused there and asked of the host (op 1): the prop's key and the item's tag. The
//             host applies it when the prop is within the sender's reach and a nametag was in the sender's hand, and
//             tells every client, the sender included, whose held item then goes as the game's own use takes it. A
//             request the host will not grant is answered to its sender alone (op 3) and the item stays.
// A joiner's tags come with its save. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropNametagPayload;
}  // namespace coop::net

namespace coop::prop_nametag {

void Install(coop::net::Session* session);

// Registers the watch on the item's use and settles it.
void Tick();

// A PropNametag arrived: a client's request on the host, the host's tag or refusal on a client.
void OnMsg(const coop::net::PropNametagPayload& p, int senderPeerSlot);

// HOST: the tags applied from a client's request since the process started (the drill reads it).
uint64_t AppliedForClients();

void OnDisconnect();

}  // namespace coop::prop_nametag
