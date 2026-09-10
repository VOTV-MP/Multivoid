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

// A class whose save state a dedicated lane already carries declares itself here, from that
// lane's own Install. The record then skips it. Two lanes writing one field is the parallel path
// RULE 2 forbids, and the dedicated lane is the one that can hold the field properly: it has the
// compare-and-swap, the conflict window and the per-verb dirty mark that a birth-time record
// cannot have. Declared at the owner, so a lane that gains a class says so where it lives.
void DeclareClassOwnedElsewhere(const wchar_t* className);

// True when this actor's class carries save state of its own AND no other lane has claimed it.
// Cheap after the first call per class; safe to ask on a birth path.
bool Covers(void* actor);

// Publish `actor`'s record. On the host that is PropSaveData to every peer; on a client it is
// PropSaveDataIntent to the host, whose re-publish is also the acknowledgement. False when the class
// is not covered, no peer could receive it, the capture fails, or the send did not take every chunk.
// A caller may ignore it: a refused send is registered and re-sent from the live actor on a widening
// backoff, and only a give-up past that ceiling is logged as a divergence.
bool Publish(coop::net::Session* s, void* actor, const std::wstring& key);

// The same record to ONE peer (the join seed). Host side.
bool PublishToSlot(coop::net::Session* s, int peerSlot, void* actor, const std::wstring& key);

// The two above with the Key taken off a wire row, so a call site that has just sent a spawn or
// an intent adds one line rather than a conversion. peerSlot < 0 is every peer.
bool PublishWithSpawn(coop::net::Session* s, void* actor, const coop::net::WireKey& key,
                      int peerSlot = -1);

// The host spawned this prop FROM a client's intent, so the client's record for it is still in
// flight behind that intent. Until it lands, the host must not publish a record for this key: what
// it would publish is the class default it just spawned, and every peer -- the author included --
// would take that over the state the author actually has. `senderSlot` is who is owed. Cleared when
// the record arrives, when that peer goes, at teardown, or on a timeout -- an intent can be lost to
// a refused rate window or a malformed body, and a key nobody clears would silence this lane's
// publisher for that prop for the whole session, the join seed included.
void ExpectRecordFor(const std::wstring& key, uint8_t senderSlot);

// A chunk of either kind off the wire. `intent` distinguishes PropSaveDataIntent (a client's
// claim, which only the host acts on) from PropSaveData (the canonical). The session is the one
// the host re-publishes a taken claim on.
void OnChunk(coop::net::Session& s, const coop::net::BlobChunkPayload& p, uint8_t senderSlot,
             bool intent);

// Apply a parked record to `actor`, whose Key is now readable; called at every mirror-birth seam.
// Unbudgeted, unlike the park drain: a newborn mirror must be right at birth, and the birth rate is
// already paced by whatever seam produced it.
// A record for a prop this peer has not created yet is parked BY KEY, with no expiry and no retry
// count: an element id names an actor, and the point of the lane is that the actor is destroyed
// and remade, while a Key survives that. The park is capped by count and an eviction is loud,
// because a bound on memory is not a deadline on an identity.
bool ApplyParked(void* actor, const std::wstring& key);

// Per frame: spends a bounded apply budget on the park (an apply is a ProcessEvent into the prop's
// own loadData, so a join's worth of them in one frame is a stall), and sweeps the chunk
// assemblers at 1 Hz. The parked records are never swept -- only applied (see above).
void Drive();

void OnDisconnect();

// Drop every parked record from one sender slot (a recycled slot must not inherit them).
void OnPeerGone(uint8_t senderSlot);

}  // namespace coop::prop_save_data
