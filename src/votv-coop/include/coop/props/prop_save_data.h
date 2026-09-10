// coop/props/prop_save_data.h -- a prop's OWN save record on the wire, addressed by its Key.
//
// The prop lane moved an actor's identity and its class-default appearance and none of its
// mutable save payload, so every route that destroys and re-creates the actor -- a pocket
// pickup, a hand switch, a device insert or eject -- lost that payload on at least one peer,
// and whichever peer re-authored the prop won with an empty copy.
//
// Three properties, each answered by the game rather than by a table kept here: capture is the
// actor's own getData, apply is its own loadData, and membership is "this class declares a
// getData of its own below Aprop_C". That last is why this is not a class list -- a list goes
// stale the first time VOTV adds a save-backed prop, and it goes stale silently.
//
// Authority: the host publishes; a client that authors a prop's state sends an intent the host
// validates and re-publishes. Game thread for capture and apply (both dispatch UFunctions).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::prop_save_data {

// Mid-join needs no seed of its own: the prop snapshot's drain already walks every prop to the
// joiner, paced over ticks, and each record ships behind its own spawn row there. A seed beside
// it was a second walk of the same set, and its burst was what refused chunks on the wire.

// True when this actor's class carries save state of its own, i.e. the lane covers it. Cheap
// after the first call per class; safe to ask on a birth path.
bool Covers(void* actor);

// Publish `actor`'s record. On the host that is PropSaveData to every peer; on a client it is
// PropSaveDataIntent to the host, whose re-publish is also the acknowledgement. False when the
// class is not covered, the capture fails, or the send did not take every chunk -- a caller must
// not ignore that: a dropped record on this lane is a permanent divergence.
bool Publish(coop::net::Session* s, void* actor, const std::wstring& key);

// The same record to ONE peer (the join seed). Host side.
bool PublishToSlot(coop::net::Session* s, int peerSlot, void* actor, const std::wstring& key);

// The two above with the Key taken off a wire row, so a call site that has just sent a spawn or
// an intent adds one line rather than a conversion. peerSlot < 0 is every peer.
bool PublishWithSpawn(coop::net::Session* s, void* actor, const coop::net::WireKey& key,
                      int peerSlot = -1);

// A chunk of either kind off the wire. `intent` distinguishes PropSaveDataIntent (a client's
// claim, which only the host acts on) from PropSaveData (the canonical). The session is the one
// the host re-publishes a taken claim on.
void OnChunk(coop::net::Session& s, const coop::net::BlobChunkPayload& p, uint8_t senderSlot,
             bool intent);

// Apply a parked record to `actor`, whose Key is now readable; called at every mirror-birth seam.
// A record for a prop this peer has not created yet is parked BY KEY, with no expiry and no retry
// count: an element id names an actor, and the point of the lane is that the actor is destroyed
// and remade, while a Key survives that. The park is capped by count and an eviction is loud,
// because a bound on memory is not a deadline on an identity.
bool ApplyParked(void* actor, const std::wstring& key);

// ~1 Hz: sweeps the chunk assemblers. The parked records are NOT swept (see above).
void Drive();

void OnDisconnect();

// Drop every parked record from one sender slot (a recycled slot must not inherit them).
void OnPeerGone(uint8_t senderSlot);

}  // namespace coop::prop_save_data
