// coop/dev/genp_probe.h -- [dev] a player fuels and starts a portable generator (VOTVCOOP_GENP_PROBE=host|client names
// the player), every peer logging the generator's fuel and state, for the Trello card "refuelling not synced to host".
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::genp_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::genp_probe
