// coop/interactables/door_verb_intent.h -- a client's own press, hit or pry of a base door runs on
// the host.
//
// A player's use of a door enters it through an entry verb -- actionOptionIndex (the press),
// addDamage (a melee hit), door_pryable_C::crowbarOpen (a crowbar's pry). On a client the script-body
// gate refuses those bodies on a door the door lane indexes and sends the verb here; the host runs
// the same verb on its own copy, where the door's body decides it once (its power gate and blackout
// clause, a swing already moving, the pry), and the result reaches every peer as DoorState. A damage
// the client's own player did not author is refused without a send: a world event is the host's to
// run. A door the lane does not index keeps its native verbs. Why the cut is at the entry verbs:
// docs/devices.md. MTA precedent: a client's vehicle entry is a request the server checks, distance
// included, and runs (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3018,
// Packet_Vehicle_InOut).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct DoorVerbIntentPayload;
}  // namespace coop::net

namespace coop::door_verb_intent {

// Registers the gate's three name watches. The per-tick retry pump (subsystems::Install), so a
// watch the gate refused is registered again on a later tick. Game thread.
void Install(coop::net::Session* session);

// Both roles: settles the watches and says once when all three are live. Host: runs the queued
// verbs, each sender at a bounded rate. Game thread, once per pump tick.
void Tick(coop::net::Session& session);

// Host: a client's verb from the wire, its format already checked by the dispatcher. Queued, and
// run by Tick; a full queue refuses it. Game thread.
void OnDoorVerbIntent(coop::net::Session& session, const coop::net::DoorVerbIntentPayload& payload,
                      uint8_t senderSlot);

// A peer left: its queued verbs and its rate go with it.
void OnPeerLeft(uint8_t slot);

// The session ended: the queues, the rates and the counters, with one summary line.
void OnDisconnect();

}  // namespace coop::door_verb_intent
