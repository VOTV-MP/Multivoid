// coop/props/bucket_sync.h -- a mop bucket's water and a concrete bucket's concrete are the host's, on every peer.
//
// prop_bucket_C holds its water as Height (with soap and brewing alcohol), changed by its tick (poured when tipped,
// filled under water, brewing) and by verbs (filled, thrown out, a plant watered, a sponge or jar dipped). A cement bag
// on one with water makes it a prop_concreteBucket_C (lib.replaceProp), whose units a trowel takes one at a time and a
// tip five, that dries in 600 s and turns back into a mop bucket when empty. On BucketState and BucketIntent:
//   HOST   -- tells every client a bucket when it changes: after its own tick, or the verb that changed it (its player's
//             cement bag, a client's ask, which it judges and runs on its own copies), ten rows a second at most for one
//             bucket; nothing runs while none changes. A joiner gets every bucket at its world-ready.
//   CLIENT -- puts each row on its copy. Its concrete buckets' tick does not run (the spill, the drying and the turn back
//             into a mop bucket are the host's). Its mop buckets' tick runs, for the water's look while tipped; a change
//             to its copy outside that tick (one of its own verbs) is asked of the host as the water and soap the verb
//             left. Its player's cement bag on a bucket and trowel on a concrete bucket are refused there and asked of
//             the host, the trowel taking its five at once. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BucketStatePayload;
struct BucketIntentPayload;
}  // namespace coop::net

namespace coop::bucket_sync {

// Cache the session. Idempotent.
void Install(coop::net::Session* session);

// Per gameplay tick: the watches' names resolve until they are live; the host sends the buckets marked changed; a
// client sends its asks.
void Tick();

// HOST: slot `slot`'s world is up (the connect replay): every bucket the host holds, to it alone.
void OnPeerWorldReady(int slot);

// HOST: a client's ask.
void OnIntent(int slot, const coop::net::BucketIntentPayload& p);

// CLIENT: the host's buckets.
void OnState(const coop::net::BucketStatePayload& p);

// Session end.
void OnDisconnect();

}  // namespace coop::bucket_sync
