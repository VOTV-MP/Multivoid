// coop/world/dug_hole_sync.h -- one set of shovel holes in the ground: the host's. See docs/piles.md.
//
// A shovel's dig into sand, dirt, gravel or grass spawns a dirthole_C (ue_wrap/actors/dig_tool). The hole is in no
// save, digs up a buried item it overlaps at its begin play, and stands until a buried item, a planted seed or a dirt
// ball ends it. The host numbers each of its dug holes at the birth the engine announces (coop/world/ambient_births)
// and sends its place and ground, its end, and every standing hole to a joiner. A client's copy is spawned with its
// begin play refused, so what a hole digs up is the host's alone. A client's own dig is refused at the same begin play,
// destroyed and sent as a DugHoleIntent; the host makes the hole for a sender holding a shovel on the host's mirror of
// its hand, within reach, inside that sender's feedback scope. A copy a client's own use ended is named to the host,
// which ends its hole for a sender within reach.
//
// Gameplay and network layer (principle 7): engine reads and spawns go through ue_wrap. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct DugHoleStatePayload;
struct DugHoleIntentPayload;
}  // namespace coop::net

namespace coop::dug_hole_sync {

// Stores the session and installs the begin-play gate, retried by Tick until it takes. Game thread.
void Install(coop::net::Session* session);

// Both roles, once per pump tick. HOST: births and ends drained and sent; each client's queued intents served at their
// budget. CLIENT: births and ends drained (a copy that ended here is named to the host), its own holes destroyed and sent,
// and the host's states applied once the world is announced ready. Game thread.
void Tick(coop::net::Session& session);

// CLIENT: the host's hole or its end, from the wire (length checked by the dispatcher). Applied by Tick.
void OnState(const coop::net::DugHoleStatePayload& p);

// HOST: a client's intent from the wire (length, role and slot checked by the dispatcher). Queued for Tick.
void OnIntent(coop::net::Session& session, int senderSlot, const coop::net::DugHoleIntentPayload& p);

// HOST: every standing hole again, to that slot alone. Called from the connect replay.
void QueueConnectBroadcastForSlot(int slot);

// The session ended: a client's copies go, the gate disarms, every queue clears. Game thread.
void OnDisconnect();

// Read-only, for the drill. A read of this session's totals.
struct Counters {
    uint64_t sent = 0;        // HOST: state messages sent
    uint64_t made = 0;        // HOST: holes made for a client
    uint64_t endedFor = 0;    // HOST: holes ended for a client
    uint64_t denied = 0;      // HOST: intents refused
    uint64_t stamped = 0;     // CLIENT: its own holes refused at begin play
    uint64_t intents = 0;     // CLIENT: intents sent
    uint64_t copies = 0;      // CLIENT: copies spawned
    uint64_t wireEnds = 0;    // CLIENT: copies ended by the host's word
};
Counters Read();

}  // namespace coop::dug_hole_sync
