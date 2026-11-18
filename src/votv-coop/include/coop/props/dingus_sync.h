// coop/props/dingus_sync.h -- the dingus (Maxwell) found once for every player.
//
// prop_dingus_C plays its music on `ws1`, an audio component active from its spawn, until it is found: the first grab
// (playerGrabbed -> Touch) sets `isused` and deactivates ws1, and its own loadData does the same for a record whose
// `isused` is set. Both are one peer's: the grab runs on the grabber's machine alone, and the record of a dingus born
// on another peer (a drop from the inventory) is not applied there, the dingus being a food the food lane owns. So the
// others went on hearing the music of a dingus someone had found (Ghezra/Multivoid#37).
//
// The found set is the host's, by the dingus's save key, which a dingus keeps through the inventory. Every peer looks
// at its dingus copies twice a second: the host puts a found one's key in the set and tells every client
// (DingusFound op 1); a client tells the host of one it found (op 0), which the host puts in the set and tells every
// client. Each peer marks every copy under a key of the set found, as the dingus's own Touch does, so a copy born later
// is marked when it is first looked at; a joiner gets the set before its snapshot.
//
// Gameplay and network layer (principle 7); engine access through reflection. Game thread.

#pragma once

namespace coop::net {
class Session;
struct DingusFoundPayload;
}  // namespace coop::net

namespace coop::dingus_sync {

void Install(coop::net::Session* session);

// Twice a second: the copies looked at, a newly found one put in the set (the host) or reported (a client), a copy
// under a key of the set marked found.
void Tick();

// A DingusFound arrived: a client's report on the host, the host's word on a client.
void OnMsg(const coop::net::DingusFoundPayload& p, int senderPeerSlot);

// HOST: every key of the set to a joiner, before its snapshot.
void SendJoinFound(int slot);

void OnDisconnect();

}  // namespace coop::dingus_sync
