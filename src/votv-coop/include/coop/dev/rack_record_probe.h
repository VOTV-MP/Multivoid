// coop/dev/rack_record_probe.h -- [dev] every drive box's record against the hand lane's bounds
// (VOTVCOOP_RACK_RECORD_PROBE=1 on the host), for VOTV-MP/Multivoid#192.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::rack_record_probe {

// The dev lanes' tick. A latched read when the probe is off. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::rack_record_probe
