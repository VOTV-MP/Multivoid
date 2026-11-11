// coop/interactables/atv_passenger.h -- this machine's player in an ATV's passenger spot, behind the driver where a
// riding kerfur sits. The use key on an ATV another player drives asks the host's table for the spot (AtvSeat, seat
// 1); granted, the player takes it through the game's own chair sit on a sit box attached to the ATV
// (ue_wrap/devices/atv_passenger_seat.h), its capsule query only so the ATV's corrections cannot knock it down. The
// player looks around and nothing else: the use key does nothing while it rides. The player's own dismount key, a
// ragdoll or the ATV's end unseats it, and the leave goes to the host. Game thread. Overview: docs/vehicles.md.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::atv_passenger {

// Store the session. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// The use key on `atv`, whose driver's seat another player holds by the host's table. True when the press is the
// passenger's: the spot is free by the table and this machine's ask follows on its next Tick; the caller swallows the
// press. False when the spot is another's, so the caller's busy deny stands. Game thread (the use input's observer).
bool OnUseAtOccupied(void* atv);

// True while the local player rides as a passenger or its ask is in flight: the use key does nothing then.
bool Riding();

// CLIENT: the host's answer to this machine's passenger ask, routed by atv_body.cpp. Game thread.
void OnAnswer(void* atv, bool admitted, uint8_t reason);

// The ask, the seat taken on the grant, and the seat's end. Game thread, every net-pump tick.
void Tick();

// The session ended: a seated player is set down and the sit box removed.
void OnDisconnect();

}  // namespace coop::atv_passenger
