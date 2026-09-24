// coop/interactables/door_verb_intent.h -- a client's own press, hit or pry of a base door runs on
// the host.
//
// A base door's state is the host's (DoorState, coop/interactables/interactable_sync), and a
// client's copy is render-only. A player's use of a door enters it through one of its entry verbs:
// actionOptionIndex (the press), addDamage (a melee hit) and, on the pryable door,
// door_pryable_C::crowbarOpen (a crowbar's pry). On a client the script-body gate refuses those
// bodies on a door the door lane indexes and sends the verb here; the host runs the same verb on its
// own copy, where the door's body decides it once, on the authority: the power gate with its
// blackout clause, a swing already moving, the alienated door's fake gray, the pry. The result
// reaches every peer as DoorState. The cut is at the entry verb and not at doorOpen or doorClose:
// every in-door caller reaches those through the door's own event graph, where a press cannot be
// told from a hit or a trigger, and a hit moves both panels before it ever reaches doorOpen.
//
// A damage the client's own player did not author (a creature, an explosion, the cheat menu) is
// refused on the client without a send, since the host's own world runs that event on its copy.
// A door the lane does not index keeps its native verbs: the lane has no name for it.
// MTA precedent: a client's vehicle entry is a request the server checks, its distance included,
// and runs (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3018, Packet_Vehicle_InOut).

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
