// coop/world/world_sound_sync.h -- world sounds the game plays in 2D on the machine that ran them alone.
//
// The earth teleport's two sounds come from the sky's tp, an event only the host's scheduler fires: the host watches its
// own PlaySound2D for them and each client plays the same sound, as EventCue ids above the emitter cues. A faucet's click
// is its presser's alone, while its state already crosses, so a peer that takes a faucet's change plays the click at the
// faucet. The shape follows Watchers of the Void's lane.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::world_sound_sync {

// EventCue ids: the emitter cues hold the ids below these, and the registry there is append-only.
inline constexpr uint32_t kCueEarthIn = 100;
inline constexpr uint32_t kCueEarthOut = 101;

void Install(coop::net::Session* session);

// CLIENT: the host's cue. False when the id is not this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// A peer's faucet press has just changed this faucet here.
void OnFaucetChanged(void* faucet);

void OnDisconnect();

}  // namespace coop::world_sound_sync
