// coop/dev/kgamer_probe.h -- [dev] a gamer Kerfur Omega made by the host and turned off and on, logged on every peer
// (VOTVCOOP_KGAMER_PROBE=1 on every peer), for VOTV-MP/Multivoid#185.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kgamer_probe {

// The dev lanes' tick. A latched read when the probe is off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::kgamer_probe
