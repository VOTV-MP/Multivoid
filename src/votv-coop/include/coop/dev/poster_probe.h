// coop/dev/poster_probe.h -- [dev] the posters on the rig (VOTVCOOP_POSTER_PROBE=1): the host puts one up, a client takes
// the nearest one down, and both log every placed poster and poster item.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::poster_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::poster_probe
