// coop/props/prop_nametag.h -- a prop's nametag is the host's, on every peer.
//
// The nametag item writes its tag into the prop the player's left click hits and builds the prop's floating label
// (ue_wrap/actors/nametag). That ran wherever the click was and nowhere else: a client's tag stayed on its own copy,
// and a tag the host put on reached no client, since a prop's nametag crosses only inside its spawn row. Now:
//   HOST   -- its player's tag runs the game's own; the watch on the item's exit tells every client.
//   CLIENT -- its player's click is refused there and asked of the host: the prop's key and the item's tag. The host
//             applies it when the prop is within the sender's reach and a nametag was in the sender's hand, and tells
//             every client, the sender included, whose held item then goes as the game's own use takes it. A refused
//             request has no answer; the item stays.
// The message is a second body shape on prop_save_data's kinds (PropSaveData from the host, PropSaveDataIntent from a
// client), first byte kBodyVersion: [2][key length][key][author slot][tag length][tag, UTF-16LE]. A joiner's tags come
// with the save and the spawn rows. Game thread.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::prop_nametag {

inline constexpr uint8_t kBodyVersion = 2;

void Install(coop::net::Session* session);

// Registers the watch on the item's use and settles it.
void Tick();

// A nametag body off either of prop_save_data's kinds: `intent` a client's request (the host acts on it), else the
// host's word (a client applies it).
void OnBody(coop::net::Session& s, const std::vector<uint8_t>& body, uint8_t senderSlot, bool intent);

// HOST: the tags applied from a client's request since the process started (the drill reads it).
uint64_t AppliedForClients();

void OnDisconnect();

}  // namespace coop::prop_nametag
