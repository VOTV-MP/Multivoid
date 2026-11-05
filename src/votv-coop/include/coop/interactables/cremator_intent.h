// coop/interactables/cremator_intent.h -- a client's press of the cremator's handle runs on the host.
//
// The handle is pulled at actionOptionIndex(player, hit, 4) with the look-at on the handle. On a client the
// script-body gate refuses that body when the press is the handle's and sends it here; the host resolves the
// cremator by its lane key, judges the sender's reach and that its own handle is at rest (the machine's own guard,
// which sits at the press and not in useLever), and pulls its lever. The run crosses by cremator_state, so every
// copy shows it. The host does not replay the press itself: its look-at is the host player's own, so the judge
// calls useLever directly; the 2D lever sound and the "door is open" hint of a replayed press stay host-local cues.
// The valve and every other action of the cremator run natively here. MTA precedent: a client's use of a shared
// element is a request the server checks and runs (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3018,
// Packet_Vehicle_InOut), as the door lane's press.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeyedTogglePayload;
}  // namespace coop::net

namespace coop::cremator_intent {

// Registers the gate's name watch. The per-tick retry pump (coop/interactables/verb_lanes), so a watch the gate
// refused is registered again on a later tick. Game thread.
void Install(coop::net::Session* session);

// Both roles: settles the watch and says once when it is live. Host: runs the queued presses in order, each sender at
// a bounded rate from its own IntentQueue; a sender whose first pose has not arrived has no body to measure its reach
// from, and its presses wait for it. Game thread, once per pump tick.
void Tick(coop::net::Session& session);

// Host: a client's press from the wire, its format already checked by the dispatcher. Queued, and run by Tick; a
// full queue refuses it. Game thread.
void OnIntent(coop::net::Session& session, const coop::net::KeyedTogglePayload& payload, uint8_t senderSlot);

// The dispatcher refused a press of this kind, for this reason: counted, not logged, so a flood costs no line. A
// payload that is not the struct's size, or a key longer than the wire's.
enum class Drop : uint8_t { Size, Key };
void CountDropped(Drop why);

// The session ended: the counters, with one summary line. A sender's queue, its rate and its said-once latches live
// in its per-slot state and go with its occupant.
void OnDisconnect();

}  // namespace coop::cremator_intent
