// coop/interactables/cremator_intent.h -- a client's press of the cremator's handle runs on the host.
//
// The handle is pulled at actionOptionIndex(player, hit, 4) with the look-at on the handle. On a client the
// script-body gate refuses that body when the press is the handle's and sends it here, carrying the state the press
// wants (`action`: 1 = the run on, 0 = off; the opposite of the client's copy). The host resolves the cremator by its
// lane key and judges the sender's reach, that its own handle is at rest (the machine's own guard, which sits at the
// press and not in useLever), that the machine is not already in the wanted state, and, for a start, that its door
// is shut; then it pulls its lever. The run crosses by cremator_state, so every copy shows it. The host does not
// replay the press itself: its look-at is the host player's own, so the judge calls useLever directly; the 2D lever
// sound of a replayed press is not played. The valve's press is refused by cremator_client_guard; every other action
// of the cremator runs natively here.
// MTA precedent: a client's use of a shared element is a request the server checks against its own state and runs
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3018, Packet_Vehicle_InOut), as the door lane's press.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeyedTogglePayload;
}  // namespace coop::net

namespace coop::cremator_intent {

// Registers the gate's name watch, once per session start (coop/interactables/verb_lanes). A watch the gate refused
// is final for the process and said once. Game thread.
void Install(coop::net::Session* session);

// Both roles: settles the watch and says once when it is live. Host: runs the queued presses in order, each sender at
// a bounded rate from its own IntentQueue; a sender whose first pose has not arrived has no body to measure its reach
// from, and its presses wait for it. Game thread, once per pump tick.
void Tick(coop::net::Session& session);

// Host: a client's press from the wire, its format already checked by the dispatcher. Queued, and run by Tick; a
// full queue refuses it. Game thread.
void OnIntent(coop::net::Session& session, const coop::net::KeyedTogglePayload& payload, uint8_t senderSlot);

// The dispatcher refused a press of this kind, for this reason: counted, not logged, so a flood costs no line. A
// payload that is not the struct's size, a key longer than the wire's, or a wanted state that is not 0 or 1.
enum class Drop : uint8_t { Size, Key, Wanted };
void CountDropped(Drop why);

// Host, dev: how many presses the judge has refused for this reason since the session began, counted whether or not
// the DENY line was said (that line is rate-limited). The drill reads them to show why a press did not run. Game
// thread. NotAuthorized counts every refusal of the authorisation (no row, a dead sender, the wrong type, no target,
// out of reach), not distance alone; HandleBusyOrUnread counts a handle mid-travel and one whose animation cannot be
// read.
enum class Denied : uint8_t { HandleBusyOrUnread, NotAuthorized };
uint64_t DeniedCount(Denied why);

// The session ended: the counters, with one summary line. A sender's queue, its rate and its said-once latches live
// in its per-slot state and go with its occupant.
void OnDisconnect();

}  // namespace coop::cremator_intent
