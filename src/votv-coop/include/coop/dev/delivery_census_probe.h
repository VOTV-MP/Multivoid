// coop/dev/delivery_census_probe.h -- see the .cpp. Diagnostic only; ini-gated OFF by default
// ([dev] delivery_census=1). Counts the holders of a delivery's goods -- the drone's cargo and each
// prop_dronesack_C with its entry count -- by class, and logs when they change, so a drop-sack, a
// put-back and a sale each read as an edge on both peers.

#pragma once

#include <cstdint>

namespace coop::dev::delivery_census {

// Game thread. Self-gated on the ini key and on its own 0.5 Hz sample interval; logs only when
// the census CHANGES (plus the first sample).
void Tick(bool isHost);

}  // namespace coop::dev::delivery_census
