// coop/session/power_subsystems.h -- the power lanes' slice of the sync-module fan-out.
//
// The power panel, the grid and its generators, and the fuse towers' rows and ops. subsystems.cpp calls each
// of the four entries from the one place its own list holds this family, so the call order across the whole
// fan-out is the one it always had.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::subsystems::power {

// The family's observer installs; retried each tick, each lane's own Install short-circuits once done.
// Game thread only.
void Install(coop::net::Session& session);

// A peer left: what each lane held for its slot is dropped. Game thread only.
void OnPeerLeft(uint8_t slot);

// The family's per-tick Tick calls, in the fan-out's order. Game thread only.
void Tick();

// The family's session teardown calls, in the fan-out's order. Game thread only.
void OnDisconnect();

}  // namespace coop::subsystems::power
