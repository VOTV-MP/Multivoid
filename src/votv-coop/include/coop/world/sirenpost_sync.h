// coop/world/sirenpost_sync.h -- the siren post stands in one place for every player.
//
// sirenpost_C (level-placed) walks on its own tick while it is not on screen: each frame it was not rendered in the
// last 60 s it steps along its `tangent`, traced to the ground, and turns its `normal` off what it meets. Every machine
// ran its own walk by its own view, so each player's post stood somewhere else, and so did its radar dot.
// HOST: its post walks; its place is sent each 0.5 s it has moved (EventCue 125) and its normal's turn with it (126,
// x, y, z = pitch, yaw, roll). CLIENT: the post's tick is refused, and its copy takes the host's place and turn. The
// host's walk reads the host's view alone: a post a client looks at may still move. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::sirenpost_sync {

// The EventCue ids; the cue registry's ids are append-only.
inline constexpr uint32_t kCueSirenPostAt = 125;    // the post's place
inline constexpr uint32_t kCueSirenPostTurn = 126;  // its normal's world rotation, sent before its place

void Install(coop::net::Session* session);

// The watch, retried until registered; the host's send. Game thread, once a tick.
void Tick();

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

void OnDisconnect();

}  // namespace coop::sirenpost_sync
