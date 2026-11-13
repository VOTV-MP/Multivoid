// coop/session/desk_subsystems.h -- the desk's slice of the sync-module fan-out.
//
// The base computer and what hangs on it: the coords panel, the desk's input and sound, the deck, the
// physical modules, the drives and their rack, the ping, the download sim, the dishes, the SAT console,
// the tape caddy and the daily task. subsystems.cpp calls each of the three entries from the one place
// its own list holds this family, so the call order across the whole fan-out is the one it always had.

#pragma once

namespace coop::net { class Session; }

namespace coop::subsystems::desk {

// The family's observer installs; retried each tick, each lane's own Install short-circuits once done.
// Game thread only.
void Install(coop::net::Session& session);

// The family's per-tick Tick calls, in the fan-out's order. Game thread only.
void Tick();

// The family's session teardown calls, in the fan-out's order. Game thread only.
void OnDisconnect();

}  // namespace coop::subsystems::desk
