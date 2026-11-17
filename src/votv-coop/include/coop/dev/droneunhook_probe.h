// coop/dev/droneunhook_probe.h -- [dev] the host unhooks the drone's sack (VOTVCOOP_DRONEUNHOOK_PROBE=1 on both
// peers), every peer logging the drone's sack.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::droneunhook_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::droneunhook_probe
