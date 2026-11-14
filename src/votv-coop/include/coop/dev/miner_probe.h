// coop/dev/miner_probe.h -- [dev] the GPU mining rig on the rig (VOTVCOOP_MINER_PROBE=1), for VOTV-MP/Multivoid issue 43:
// the host builds a rig up, a client claims its points and adds a GPU, and both log every rig's upgrades.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::miner_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::miner_probe
