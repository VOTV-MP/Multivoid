// coop/creatures/firetank_sync.h -- the Firetank's flame shows on every player's machine.
//
// firetank_C crosses as a world-actor mirror whose drive, sight and fire are refused on a client
// (coop/world/spawn_authority), so its flamethrower (a flamethrower_C actor it holds) never lit there. The flame's
// Activate(bNewActive) shows its effect and its loop and switches on its tick, whose traces set alight what it
// touches; the host's burn of a client's body reaches that client as its own ignite (coop/player/player_damage).
// HOST: a flamethrower's Activate is sent with the Firetank's place (EventCue 122 on, 123 off). CLIENT: the copy's
// flame shows or stops its effect and its loop, its tick left off, so it burns nothing here. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::firetank_sync {

// The EventCue ids; the cue registry's ids are append-only.
inline constexpr uint32_t kCueFlameOn = 122;    // the Firetank's place
inline constexpr uint32_t kCueFlameOff = 123;   // the Firetank's place

void Install(coop::net::Session* session);

// The watch, retried until registered. Game thread, once a tick.
void Tick();

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

void OnDisconnect();

}  // namespace coop::firetank_sync
