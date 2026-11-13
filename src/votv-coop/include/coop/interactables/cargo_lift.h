// coop/interactables/cargo_lift.h -- the cargo lift, the base's elevator, is the host's.
//
// The lift's direction (`dir`) is the host's state, sent to every client on the cargo lift channel
// (coop/interactables/interactable_sync, CargoLiftState). A press of the call buttons runs the lift's own
// actionOptionIndex: on the host it runs, and the direction it leaves is sent from the press's end; on a client it is
// refused and sent to the host as the direction it wants (CargoLiftIntent). The host judges the sender's life, reach and
// that its own lift is not moving, and presses its lift. The lift also reverses itself when its platform is blocked
// (the Blueprint's move loop, `ignore`), and the host sends that direction too. A client applies the host's direction
// by pressing its own copy, and turns a copy that moves by writing `dir`. Game thread.
//
// Divergence from MTA, which sends a timed animation every client interpolates and a joiner the running one
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CObject.cpp:336-361): the lift is a Blueprint timeline that takes no
// animation, so each copy runs its own travel from the direction it is told and the copies meet at the stops; a joiner
// mid-move runs the whole travel. A press while it moves is ignored, as Source does
// (reference/source-sdk-2013/src/game/server/buttons.cpp:212-215), where MTA redirects the object.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct KeyedTogglePayload;
}  // namespace coop::net

namespace coop::cargo_lift {

// Registers the press's watch. Called every pump tick and latched. Game thread.
void Install(coop::net::Session* session);

// Both roles: settles the watch. Host: runs the queued presses, each sender at a bounded rate; a sender with no body
// yet waits for it. Game thread, once per pump tick.
void Tick(coop::net::Session& session);

// Host: a client's press from the wire (CargoLiftIntent), its format checked by the dispatcher. Queued and run by Tick. Game thread.
void OnIntent(coop::net::Session& session, const coop::net::KeyedTogglePayload& payload, uint8_t senderSlot);

// The channel's read and apply of a lift's direction (interactable_sync's cargo lift adapter). The apply presses this
// copy when its direction differs and it stands still, or turns it at once by writing `dir` when it moves.
bool ReadState(void* lift, bool& up);
bool ApplyState(void* lift, bool up);

// How many of this client's presses went to the host as an intent since the session began. The press's gate sends inside
// the call itself, so a drill reads it before and after its own press. Game thread.
uint64_t PressesSent();

// How many were cancelled and not sent (the lift not indexed yet, a direction that does not read, a send the session
// refused): a press that is neither sent nor run alone, which a drill must not read as a press that ran alone.
uint64_t PressesCancelled();

// The session ended: the counters, with one summary line.
void OnDisconnect();

}  // namespace coop::cargo_lift
