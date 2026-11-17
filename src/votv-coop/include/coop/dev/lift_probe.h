// coop/dev/lift_probe.h -- [dev] a client standing on the cargo lift while the host sends it up and back down
// (VOTVCOOP_LIFT_PROBE=1 on both peers); both peers log the lift's direction and its platform's height.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::lift_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::lift_probe
