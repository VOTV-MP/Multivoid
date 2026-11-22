// coop/creatures/scout_target.h -- the Rozital scouts hunt and pin any player, not only the host's.
//
// tentacleBall_C (the tentacleBalls event, day 32) runs on the host alone; a client holds mirrors. A hostile ball that
// sees a player chases player 0, the host, and once within 40 m pins the first player its sphere finds: a check that
// player 0's camera has open sky above it, then `firstPersonBodyAnimation("pindown")` on that player. A client's puppet
// on the host was seen, and the chase went to the host; a pinned puppet played the pin on the host's copy alone.
// HOST: each hostile ball not yet chasing is served the nearest player (coop/creatures/served_player, its
// GetPlayerPawn(0) read), kept while it chases and pins, so its chase, its sky check and its lunge go to that player;
// the camera read of its first sky check answers the same body. A pin of a client's puppet is refused on the host and
// sent to that client (EventCue 109). CLIENT: its own player plays the pin. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::scout_target {

// The EventCue id; the cue registry's ids are append-only. Sent to the pinned client alone.
inline constexpr uint32_t kCueScoutPin = 109;

void Install(coop::net::Session* session);

// HOST: the balls' targets, five times a second, and the watch and hook retried until registered. Game thread.
void Tick();

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

void OnDisconnect();

}  // namespace coop::scout_target
