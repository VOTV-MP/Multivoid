// coop/dev/glowstick_probe.h -- [dev] a client sticks a glowstick to the floor (VOTVCOOP_GLOWSTICK_PROBE=1 on both
// peers), every peer logging the glowstick's place and its stick, for the Trello card "Desynchronization of placed
// items in multiplayer".
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::glowstick_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::glowstick_probe
