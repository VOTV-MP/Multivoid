// coop/dev/weather_probe.h -- the ini-gated weather diagnostics (dev tooling,
// RULE-2-exempt).
//
// [probe weather]: ~1 Hz cycle rain+fog observable state + eff_rain IsActive
// on BOTH peers (ini weather_probe=1). [probe wind]: the visible-wind input
// chain + the roll interceptor counters. Comparing host vs client lines
// settles Activate-vs-param rain questions and wind-desync classes; the
// acceptance gates of the modularization cuts also consume these lines for
// cross-peer parity checks.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::weather_probe {

// Per-tick worker; call from weather_sync::TickConnect. Cheap when the ini
// flag is off (one static bool load); resolves the cycle via
// coop::weather_rain::Cycle() only under the ini + ~1 Hz throttle gates.
// Game thread only.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::weather_probe
