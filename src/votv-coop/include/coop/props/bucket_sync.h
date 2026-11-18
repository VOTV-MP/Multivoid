// coop/props/bucket_sync.h -- a mop bucket's water and a concrete bucket's concrete are the host's, on every peer.
//
// prop_bucket_C holds its water as Height (soap and brewing alcohol beside it), changed by its own tick (poured out
// when tipped, filled under water, brewing) and by verbs: its left click fills it from water, waters a plant or throws
// the water out, a sponge dips in it, a jar takes from it. A cement bag's left click on one with water turns it into a
// prop_concreteBucket_C (lib.replaceProp), whose units the trowel's left click takes one at a time and a tip spills five
// at a time, that dries on a 600 s timer, and that turns back into a mop bucket when empty. All of it ran wherever it
// ran and nowhere else. Now, on BucketState and BucketIntent:
//   HOST   -- reads its buckets four times a second and tells every client the ones that changed, and every bucket to a
//             joiner at its world-ready; a client's ask (below) it judges and runs on its own copies.
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

// Per gameplay tick: the watches' names resolve until they are live; the host reads its buckets and sends what
// changed; a client sends its asks.
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
