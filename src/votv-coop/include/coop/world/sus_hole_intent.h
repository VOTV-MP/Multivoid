// coop/world/sus_hole_intent.h -- a sus hole is rolled by its own player's game and made by the host.
//
// The sus hole spawner runs on every peer, as the game made it: every 300 to 600 s it rolls a point 100 to 500 m around the
// camera of player 0, which is the local player on each machine, and makes a hole there if the ground is grass. A client's
// roll is its own player's, so the hole it makes is not a second set: the FinishSpawningActor hook stamps it (the spawned
// class is susDirtHole_C and the calling actor is the spawner), the tick destroys it and sends its place to the host as a
// SusHoleIntent, and the host makes the hole there if the intent is honest. The fixture lane (coop/world/ambient_fixture_sync)
// then sends that hole to every peer like any other, the rolling client included.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct SusHoleIntentPayload;
}  // namespace coop::net

// HOST: each client's intents wait in its own queue (4) and run at the spawner's own pace (a burst of 2, then one in 300 s).
// The point must lie 10000 to 50000 uu from the client's body (the spawner's ring; the outer bound adds the pose pad of
// coop/element/intent_authority, the inner one does not), the host's own down-trace must hit grass within 200 uu of the named
// height (the spawner's own test), and at most 4 holes the host made for that client may stand. A refusal loses that roll
// (the next is 300 to 600 s away), is counted by reason and said once for that client. The drone, whose camera can be far
// from the body, is the one honest roll this refuses: MTA keeps a camera position per player for such a test
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3000-3004), and the host here has a client's body alone.
// A client that leaves and joins again is a new occupant: the holes it had stand, counted by nobody, so the bound is the
// reconnect rate.
namespace coop::sus_hole_intent {

// A hole's end is not here: a hole ends by its own viewer's distance and view, which only each peer can judge for its own
// player, so no hole end crosses. The hook installs on the session's first tick, ahead of the world load, and acts only
// while this process holds an active client session.
//
// The hook's install, tried once a second up to 30 times (client role only; the host pays nothing), and armed only while this
// process holds a client session. Game thread.
void Install(coop::net::Session* session);

// Both roles, once per pump tick. CLIENT: the stamped holes are read, destroyed and sent once the world is announced ready.
// HOST: each client's queued intents are served, one head a tick at its budget. Game thread.
void Tick(coop::net::Session& session);

// HOST: a client's intent from the wire, its length checked by the dispatcher. Queued for Tick; the sender's slot range, the
// place's finiteness and the queue's cap are judged here. Game thread.
void OnIntent(coop::net::Session& session, int senderSlot, const coop::net::SusHoleIntentPayload& p);

// HOST: a dirt hole ended (its end of play, from the death seam): it no longer counts against the player it was made for.
// Game thread.
void OnHoleEnded(void* obj, int32_t index);

// The session ended: the summary line, the counters, every queue and the stamped holes, and the spawn hook disarmed. Game
// thread.
void OnDisconnect();

// Why an intent went nowhere, for the counters.
enum Reason : uint8_t { kMalformed, kQueueFull, kTooNear, kTooFar, kStanding, kNoGrass, kNotMade, kReasonCount };
const char* ReasonName(Reason why);

// Read-only, for the drill. A read of this session's totals.
struct Counters {
    uint64_t stamped = 0;   // CLIENT: holes its own spawner rolled that the hook stamped
    uint64_t sent = 0;      // CLIENT: intents sent
    uint64_t received = 0;  // HOST: intents past the dispatcher
    uint64_t accepted = 0;  // HOST: holes made
    uint64_t refused[kReasonCount] = {};  // HOST: by reason
    float sentX = 0.f, sentY = 0.f, sentZ = 0.f;  // CLIENT: where the last intent sent said its hole stood
};
Counters Read();

}  // namespace coop::sus_hole_intent
