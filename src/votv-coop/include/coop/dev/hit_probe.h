// coop/dev/hit_probe.h -- [dev] a player's melee hit on the floor (VOTVCOOP_HIT_PROBE=client|host on both peers), every
// peer logging the relayed hit sound.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::hit_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::hit_probe
