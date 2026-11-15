// coop/world/evil_ending.h -- the Evil's ending, as a moment every player gets.
//
// The Evil (theEvil_C, minted by the base computer when the `evil` signal is refined to its last level) is a countdown on
// the host alone: the desk simulation is the host's. When the countdown ends with a copy of the signal still in the world,
// its graph switches its own post process on, plays white noise and mints the game-over actor, whose trip to the menu a
// co-op session refuses and answers with a revive (coop/player/run_end_travel). A client had no part in that: it kept
// playing while the host sat behind the game-over screen. The host now sends one EventCue when its Evil's ending starts,
// and a client runs the same lines of the Evil's own graph on a stand-in of its own: the post process, the noise and the
// game-over actor with the game's own text, entered past the achievement and the save, which stay the host's. Each peer's
// refused menu trip then ends the moment: the post process is switched off and a client's stand-in is destroyed.
// Nothing is replayed to a joiner: the ending is a moment, not a state. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::evil_ending {

// The EventCue id of the ending's start; the cue registry's ids are append-only.
inline constexpr uint32_t kCueEnding = 160;

void Install(coop::net::Session* session);

// HOST: the watch for an Evil whose ending has started. CLIENT: a cue whose class was not loaded yet is tried again.
void Tick();

// CLIENT: the host's cue. False when the id is not this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// Either role: this peer's menu trip was refused (the game-over actor's among them), so an ending in progress here is over.
void OnRunEndRefused();

void OnDisconnect();

}  // namespace coop::evil_ending
