// coop/world/creature_event_relay.h -- the host's creature events, carried to every client's event count.
// lib_C::setEvent adds 1 to gamemode.activeEvents and appends its caller to activeEvents_senders when an event
// begins, and reverses both when it ends; the count drives the threat music, the sleep refusal, the save gates and
// the quit and pause blocks (lib_C::getEvent). A mirror with its brain off no longer runs the creature's own calls,
// so the host's count is the source.
//   - HOST: a watch on setEvent keeps each enrolled creature's net count of `true` calls and sends its whole count,
//     by element id, as ReliableKind::CreatureEventActive on every change, at the creature's enrolment after its own
//     `true`, and at a joiner's world-ready; a creature that ends still holding a count sends 0.
//   - CLIENT: a count is the target of the creature's mirror. The relay applies setEvent by reflection with the
//     mirror as the context until its applied count equals the target, retracts the registrations the mirror made
//     before it was installed, and refuses the mirror's own setEvent once the bind made an entry (a bind the relay
//     cannot take makes none, and that mirror counts its own). The client's count is the sum of these keyed
//     registrations and its own events; a mirror's end and the session's end retract them; a world change drops them.
//   - PER VIEWER: killerwisp_C, fossilhound_C, antibreather_C and eg_C compute their event from the local camera or
//     player 0; their mirror's own call is allowed and the host's relay skips them.

#pragma once

#include "coop/element/element.h"

namespace coop::net {
class Session;
struct CreatureEventActivePayload;
}  // namespace coop::net

namespace coop::creature_event_relay {

// Cache the session, register the setEvent watch and subscribe to the death seam, each once per process. Called
// every pump tick by the install fanout, which is also the retry until each piece latches. Game thread.
void Install(coop::net::Session* session);

// HOST, game thread, at a joiner's ClientWorldReady edge: the count of every creature that holds one, to this slot.
void SendJoinSnapshotForSlot(int slot);

// HOST, game thread: an Npc element was enrolled after its BeginPlay ran (the spawn catch, the connect-edge walk,
// the silent register). Counts the actor's `true` copies in the game's registry and sends them.
void OnNpcEnrolled(coop::element::ElementId eid, void* actor);

// CLIENT, game thread: a mirror was installed for `eid` and its BeginPlay has run. Retracts its own
// registrations, then applies the host's count; refused with a WARN, and no entry made, when setEvent, the event
// registry or the library's default object is not resolved yet.
void OnMirrorBound(coop::element::ElementId eid, void* actor);

// CLIENT, game thread (event_feed drain): a creature's count from the host.
void OnReliable(const coop::net::CreatureEventActivePayload& payload);

// Teardown: retract what a client still has applied, drop both halves' state and the session.
void OnDisconnect();

// [dev], for the drill alone: the target and the applied count of `eid`'s entry; -1 when there is none.
int TargetFor(coop::element::ElementId eid);
int AppliedFor(coop::element::ElementId eid);

}  // namespace coop::creature_event_relay
