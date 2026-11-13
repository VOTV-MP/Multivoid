// coop/dev/roomba_probe.h -- [dev] the robot cleaner on the rig (VOTVCOOP_ROOMBA_PROBE=1), for coop/props/roomba_sync:
// the host drops one and switches it on, a client later presses its switch, and both log where each cleaner is.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::roomba_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::roomba_probe
