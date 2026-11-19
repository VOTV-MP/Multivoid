// coop/dev/prop_census_drift.h -- [dev] every keyed prop near the base, where each peer has it, on a clock every peer
// shares, so a host's and a client's copies compare instant for instant (VOTVCOOP_PROP_DRIFT=1 on every peer).
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::prop_census_drift {

// The dev lanes' tick. Game thread.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::prop_census_drift
