// coop/dev/blackfog_probe.h -- [dev] the host rolls a black fog (VOTVCOOP_BLACKFOG_PROBE=1 on both peers), every peer
// logging its black fogs and eyes.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::blackfog_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::blackfog_probe
