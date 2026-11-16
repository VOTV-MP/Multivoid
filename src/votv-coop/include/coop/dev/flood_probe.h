// coop/dev/flood_probe.h -- [dev] the basement flood (VOTVCOOP_FLOOD_PROBE=1 on both peers): the host bursts a pipe, a
// client mends it, every peer logs the pipes and the flood's level.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::flood_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::flood_probe
