// coop/world/world_sound_sync.h -- world sounds the game plays in 2D on the machine that ran them alone.
//
// The earth teleport's two sounds come from the sky's tp, an event only the host's scheduler fires: a pre hook on the
// host's PlaySound2D reads the Sound argument and, for earthTP_in or earthTP_out (their objects held per world as cached refs, asked again while a world lacks them),
// sends EventCue ids above the emitter cues, which each client plays. A faucet's click is its presser's alone, while its
// state already crosses, so a peer that takes a faucet's change plays the click at the faucet. The shape follows Watchers
// of the Void's lane.

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

// Store the session, put the hook on once, find the sounds in each world (on either role: the host compares them in
// its hook, a client plays them for a cue) and arm the hook while this is a host. Disconnect disarms it. Called every
// load tick. Game thread.
void Install(coop::net::Session* session);

// CLIENT: the host's cue. False when the id is not this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// A peer's faucet press has just changed this faucet here.
void OnFaucetChanged(void* faucet);

void OnDisconnect();

// What this session did, for the drill: the host's sky sounds seen and its cues sent, a client's cues received and played, the faucet clicks
// played here. Game thread.
struct Counts {
    uint32_t matched = 0, sent = 0, received = 0, played = 0, clicks = 0;
};
Counts GetCounts();

}  // namespace coop::world_sound_sync
