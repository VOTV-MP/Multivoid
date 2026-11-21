// coop/interactables/grime_judge.h -- the host judges a client's fall of a grime decal.
//
// A decal has no syncer: any player within reach of one may clean it, so a client's fall (GrimeState, a live fall) is an
// ask the host weighs on its own, never a value it applies as it came. The client's own verb still runs at once on its
// copy (an ask per 0.25 s stroke would lag the sponge) and the lane sends its fall to the host; the kind is not relayed.
// MTA accepts a world element's change only from its syncer
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CObjectSync.cpp:214, compiled out in this tree, so the shape is the
// precedent; CUnoccupiedVehicleSync is the live syncer), so a lane with none judges
// every ask: the sender's hand holds the tool the verb needs and the decal is within its reach, or the host is itself
// raining on a decal the rain would wash and the sender's body is within the rain's radius. A grant is cut to one call of
// the game's own fall, so no ask removes a decal in one message. The host lowers its own copy and floor only after the
// grant, then sends the fall to every client but the asker; a cut ask raises the asker's copy back to the host's value, a
// refused one on a known decal likewise, and one naming no decal gets no answer.
// A client holds every destroy of its grime decals (the seam in grime_judge.cpp). docs/devices.md carries the lane and its trust line.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::grime_judge {

// What the lane did, for the drill and the session's summary. Game thread.
struct Stats {
    uint64_t asked = 0;             // HOST: falls received from clients
    uint64_t coalesced = 0;         // HOST: asks folded into a queued ask of the same key
    uint64_t granted = 0;           // HOST: falls granted as asked
    uint64_t clamped = 0;           // HOST: falls granted cut to one step (the asker was raised back)
    uint64_t ended = 0;             // HOST: falls to zero granted, the decal destroyed
    uint64_t toolGrants = 0;        // HOST: grants by the tool route (a sponge or a wall fixer in hand, within reach)
    uint64_t rainGrants = 0;        // HOST: grants by the rain route alone
    uint64_t already = 0;           // HOST: asks not below what the host holds
    uint64_t refusedKind = 0;       // HOST: an ended mark, a snapshot or no key from a client
    uint64_t refusedQueue = 0;      // HOST: the sender had kMaxPending keys queued
    uint64_t refusedNodecal = 0;    // HOST: a key naming no decal the host indexes
    uint64_t refusedNobody = 0;     // HOST: the sender took no pose within the wait
    uint64_t refusedReach = 0;      // HOST: the tool in hand, the decal out of its reach
    uint64_t refusedNotool = 0;     // HOST: no tool in hand and the host not raining
    uint64_t refusedRain = 0;       // HOST: the host raining, the rain route's own test refused it
    uint64_t refusedDead = 0;       // HOST: the sender's player is not alive
    uint64_t held = 0;              // CLIENT: destroys cancelled at the source
    uint64_t heldOff = 0;           // CLIENT: destroys left to run (the red arm's)
};

// Both roles: puts the K2_DestroyActor seam on, once its class resolves, and arms it for a client in a session. Host:
// serves each sender's queue, one head a tick, and answers the keys a full queue could not take. Game thread.
void Tick(coop::net::Session& session);

// HOST: GrimeState from a client, the dispatcher's case, before the lane's receive sees it.
void OnAsk(coop::net::Session& session, const coop::net::KeyedScalarPayload& payload, uint8_t senderSlot);

// CLIENT. The destroy of a grime decal is held (the seam above) unless a lane of ours destroys it: a scope of this type
// lets the destroys inside it through (the lane's apply of the host's end, a spill the spill lane removes). Game thread.
class AllowDestroy {
public:
    AllowDestroy();
    ~AllowDestroy();
    AllowDestroy(const AllowDestroy&) = delete;
    AllowDestroy& operator=(const AllowDestroy&) = delete;
};

// CLIENT, from the lane's watch on clean and wallFixer_fix. OpenVerb, in the PRE, forgets what the last verb held and
// returns the scope bit the lane puts on the body it lets run (script_gate::MarkScope), 0 when the hold is off. The seam
// reads that bit off the calling frame to tell a destroy inside the verb, which asks the host for zero, from any other.
// TakeHeld, in the POST, says whether the seam held `decal`'s destroy inside the verb. HoldIsOff: the seam did not go on, so a
// destroy the verb ran is already done and its POST asks zero after the fact. Game thread.
uintptr_t OpenVerb();
bool TakeHeld(void* decal);
bool HoldIsOff();

// DEV-DRILL ONLY: what the lane has done in this process, read by the drills that judge it; and the rain's roof test the
// judge runs on the host's copy, which a drill's pick of a decal the rain washes asks too (true: no WorldStatic object
// within the trace above the decal). Game thread.
Stats GetStats();
bool RoofClear(void* decal);

void OnDisconnect();

}  // namespace coop::grime_judge
