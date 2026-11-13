// coop/props/prop_nametag.h -- a prop's nametag is the host's, on every peer.
//
// The nametag item writes its tag into the prop the player's left click hits and builds the prop's floating label
// (ue_wrap/actors/nametag). That ran wherever the click was and nowhere else: a client's tag stayed on its own copy,
// and a tag the host put on reached no client. Now, on the PropNametag kind:
//   HOST   -- its player's tag runs the game's own; the watch on the item's exit tells every client (op 2).
//   CLIENT -- its player's click is refused there and asked of the host (op 1): the prop's key and the item's tag. The
//             host applies it when the prop is within the sender's reach and a nametag is in the sender's hand, and
//             tells every client, the sender included, whose held item then goes as the game's own use takes it. A
//             request the host will not grant is answered to its sender alone (op 3) and the item stays. The hand is
//             the one the hand lane has applied; a request that arrives while the sender's newest hand state is owed
//             waits for its application.
// A joiner's tags come with its save, and the ones put on since its cut with the host's connect replay (SendJoinTags).
// A prop's spawn row and record carry no nametag. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::prop_nametag {

void Install(coop::net::Session* session);

// Registers the watch on the item's use and settles it, sweeps the half-assembled messages, and retries what the key index
// could not answer yet.
void Tick();

// One chunk of a PropNametag message arrived (game thread, in the drain): when the message is whole, a client's request on
// the host, the host's tag or refusal on a client. Every length is read from the assembled body; what does not add up to
// it is refused (the host answers op 3) and never read past.
void OnChunk(coop::net::Session& session, const coop::net::BlobChunkPayload& chunk, int senderPeerSlot);

// A peer left: the chunks it had half sent go.
void OnPeerGone(uint8_t slot);

// HOST: the slot's connect replay (inside the snapshot scope, ahead of the snapshot). Each prop whose tag this session
// published is named to that slot alone, with the tag it carries now: the joiner's save is a photograph, and a tag is
// not sent to a slot before its world-ready, so a tag put on after its cut reaches it here. A prop the snapshot births
// after this is tagged when it appears (CLIENT: the tag waits by key).
void SendJoinTags(int slot);

// HOST: the tags applied from a client's request since the process started (the drill reads it).
uint64_t AppliedForClients();

// The events the nametag drill judges on, each since the session began. HOST: the host player's own tags told to the
// clients. CLIENT: the requests asked of the host, the host's refusals of them, the host's tags for them (each spends
// its item), and the host's tags of any author.
uint64_t HostPublishedCount();
uint64_t AskedCount();
uint64_t RefusedAnswerCount();
uint64_t SpentCount();
uint64_t PublishedCount();

void OnDisconnect();

}  // namespace coop::prop_nametag
