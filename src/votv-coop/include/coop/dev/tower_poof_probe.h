// coop/dev/tower_poof_probe.h -- [dev] the stolen radio tower on the rig (VOTVCOOP_TOWER_POOF_PROBE=1), for
// VOTV-MP/Multivoid issue 63: the host starts the event, and both log whether the tower shows.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::tower_poof_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::tower_poof_probe
