// coop/interactables/cargo_lift.h -- the cargo lift, the base's elevator, is the host's.
//
// The lift's direction (`dir`) is the host's state, sent to every client on the cargo lift channel
// (coop/interactables/interactable_sync, CargoLiftState). A press of the call buttons
// runs the lift's own actionOptionIndex: on the host it runs, and the direction it leaves is sent from the press's end;
// on a client it is refused and sent to the host as the direction it wants (CargoLiftIntent). The
// host judges the sender's reach and that its own lift is not moving, and presses its lift. A client applies the
// host's direction by pressing its own copy, so the lift travels there as it does on the host, and a direction that
// arrives while its copy still moves is pressed once it stops. Game thread.

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
// copy when its direction differs, or holds the direction until a moving copy stops; TickApply presses the held ones.
bool ReadState(void* lift, bool& up);
bool ApplyState(void* lift, bool up);
void TickApply();

// The session ended: the counters, with one summary line, and the held directions.
void OnDisconnect();

}  // namespace coop::cargo_lift
