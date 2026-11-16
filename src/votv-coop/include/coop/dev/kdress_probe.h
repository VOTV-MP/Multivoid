// coop/dev/kdress_probe.h -- [dev] an Omega's outfit and its pat (VOTVCOOP_KDRESS_PROBE=client|host on both peers): the
// actor puts an item on the nearest Omega, pats it and takes the item off; every peer logs each Omega's outfit.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kdress_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::kdress_probe
