// coop/creatures/walker_lanes.h -- the walking mannequin's lanes, wired in one place: a client's copy never thinks
// (walker_brain), the host claims the walker into the drive stream and chooses the player it hunts (walker_target),
// its pose and hunt cross as WalkerState (walker_state_host, walker_state_client), and it freezes on its target's gaze
// only (walker_gaze_host answers the reads, walker_gaze judges on the target's machine). Each registers its watches
// (Install), settles them and runs its queue (Tick), drops what a leaver held (OnPeerLeft), what a joiner needs at its
// world-ready edge (OnPeerWorldReady), and its session state (OnDisconnect). This calls them, in that order. Game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::walker_lanes {

// From the session's first tick (coop/session/subsystems, InstallLoadWatchers).
void Install(coop::net::Session& session);

// Per gameplay tick: the HOST claims the walkers born since the last tick; a CLIENT hears its copies end and judges its gaze.
void Tick(coop::net::Session& session, bool isHost);

// A peer's world-ready edge (a joiner's, and again at every world change the peer makes): every held walker's pose and hunt to
// `slot` alone.
void OnPeerWorldReady(int slot);

// A peer left: the walkers that hunted it choose again, and the levels it sent are dropped.
void OnPeerLeft(uint8_t slot);

void OnDisconnect();

}  // namespace coop::walker_lanes
