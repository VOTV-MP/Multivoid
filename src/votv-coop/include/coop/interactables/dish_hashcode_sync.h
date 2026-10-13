// coop/interactables/dish_hashcode_sync.h -- the dishes' hash codes are the host's.
//
// generteHashcode writes a dish's `hashcode`, the nine-line text the SAT console's sv.hash shows and
// sv.request exports onto a disc, from fresh random bytes; the day rollover is its one caller, once per
// dish in one frame. A client never rolls a midnight, so its codes stayed the ones its save carried. So
// the gate's post on the host's generteHashcode marks the dish, the next tick sends every marked dish's
// code, and a client writes them in and refuses a generteHashcode of its own. Rows for a dish a client
// has not resolved wait for it. Late join (principle 8): the transferred save carries the codes as the
// host captured them, a broadcast skips a joiner whose world is not up, and so every code goes to the
// joiner again at its world-ready, which covers a midnight between the capture and that edge. MTA's
// shape: a change goes to joined players only, a joiner gets every element's state
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1033-1034,
// reference/mtasa-blue/Server/mods/deathmatch/logic/CMapManager.cpp:195). Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dish_hashcode_sync {

// Watch generteHashcode (a host marks the dish, a client refuses it) and cache the session. Idempotent,
// retried until the gate takes the watch. Game thread.
void Install(coop::net::Session* session);

// HOST: send the marked dishes' codes, and a joiner's set. CLIENT: write the rows that waited for their
// dish. Game thread, every pump tick; idle costs a few compares.
void Tick();

// A DishHashcodes chunk: a client takes the host's rows. From the host alone: the host's receive
// edge refuses a client's copy (client_authorship.h). Game thread.
void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: a joiner's world is up -- every dish's code to it, retried until it goes whole.
void QueueConnectBroadcastForSlot(int peerSlot);

// A peer left: its half-assembled rows and its owed set go.
void OnPeerGone(uint8_t slot);

// Session end: the marks, the owed sets and the waiting rows go.
void OnDisconnect();

}  // namespace coop::dish_hashcode_sync
