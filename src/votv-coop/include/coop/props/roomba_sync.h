// coop/props/roomba_sync.h -- the robot cleaner (prop_roomba_C) is the host's: where it drives, whether it runs, and
// what it picks up.
//
// The cleaner drives itself by physics: its tick turns its wheels, a random roll and its bumper's hits pick its turns,
// and its overlap takes items out of the trash piles it passes into its own container. Each peer ran that on its own
// copy, so the copies drove apart. Here a client refuses the cleaner's tick and its pickup at the script gate. While the
// host's cleaner runs, the host claims its drive (coop/props/prop_drive_host), so every client's copy follows the host's
// pose, and a client animates the brushes itself. A client's press of the switch (action 5) is refused and sent to the
// host (RoombaIntent), which judges its reach and presses its own cleaner; the host sends every change of `active`
// (RoombaState), at a joiner's ready edge too, and a client takes it through the cleaner's own `use`. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct RoombaPayload;
}  // namespace coop::net

namespace coop::roomba_sync {

// Keep the session and register the watches on the cleaner's tick, pickup and actions. Idempotent.
void Install(coop::net::Session* session);

// HOST: claim and release the running cleaners' drives, send each `active` change, judge the queued presses. CLIENT:
// apply the host's states, animate the running copies' brushes. Settles the watches. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every cleaner's `active`, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's press, its length checked by the dispatcher.
void OnIntent(int senderSlot, const coop::net::RoombaPayload& p);

// CLIENT: the host's state, its length checked by the dispatcher.
void OnState(const coop::net::RoombaPayload& p);

// Session end: the records are cleared.
void OnDisconnect();

}  // namespace coop::roomba_sync
