// coop/dev/vent_probe.h -- [dev] a player crawling in a vent (VOTVCOOP_VENT_PROBE=client|host on both peers): the
// crawler crouches at the vent knocker and walks; the other peer logs which of its steps found ground.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::vent_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::vent_probe
