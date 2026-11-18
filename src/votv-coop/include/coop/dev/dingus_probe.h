// coop/dev/dingus_probe.h -- [dev] the dingus (Maxwell) found, pocketed and dropped by one peer, its music state logged
// on every peer (VOTVCOOP_DINGUS_PROBE=host|client names the finder), for Ghezra/Multivoid issue 37.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::dingus_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::dingus_probe
