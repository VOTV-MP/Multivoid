// coop/dev/sky_tp_probe.h -- [dev] the earth teleport on the rig (VOTVCOOP_SKY_TP_PROBE=1), for coop/world/sky_sync:
// the host fires earthTp, a client calls its own tp, and both log the sky's black hole.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::sky_tp_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::sky_tp_probe
