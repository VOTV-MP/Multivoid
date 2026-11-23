// coop/creatures/gray_sync.h -- the Grays at the transformer (graysforest) touch, and lock the ATV, on every machine.
//
// The Grays (grayTest_C) cross on the NPC lane, inert on a client. On the host, a Gray's sphere taken by any
// mainPlayer_C, a client's body included, runs its controller's Despawn, which punches lib_C::getMainPlayer -- player
// 0, the host's player -- so a client walking into them knocked the host down. The controller's begin zaps the ATV
// (car.zapped, read by its forward input) and its end clears it; the ATV's lane does not carry it, so a client's ATV
// drove on.
// HOST: while Grays stand, their controller is served a client (coop/creatures/served_player, the main-player seam,
// its watch live before a touch); a Gray taken by a client's body serves that client the punch, delivered to it
// (coop/creatures/creature_hit), and one taken by the host's body is the host's. The ATV's zapped is sent as it changes and every 5 s while set (EventCue 124, x = 1 or 0). CLIENT: its ATV copy takes it.
// Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::gray_sync {

// The EventCue id; the cue registry's ids are append-only.
inline constexpr uint32_t kCueAtvZapped = 124;   // x = 1 zapped, 0 not

void Install(coop::net::Session* session);

// The watch, retried until registered; the host's ATV poll. Game thread, once a tick.
void Tick();

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

void OnDisconnect();

}  // namespace coop::gray_sync
