// coop/dev/foam_probe.h -- [dev] a fire extinguisher's foam on the rig (VOTVCOOP_FOAM_PROBE=1), for Ghezra/Multivoid
// issue 31: a client sprays the extinguisher in its hand, the host sets a world one thrusting, and both log what each
// copy shows.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::foam_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::foam_probe
