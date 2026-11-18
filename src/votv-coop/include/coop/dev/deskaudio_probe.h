// coop/dev/deskaudio_probe.h -- [dev] whether the signal desk's sound components are heard in space
// (VOTVCOOP_DESKAUDIO_PROBE=1), for Ghezra/Multivoid issue 29: each component's spatialization and attenuation, and
// its sound's, logged once.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::deskaudio_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::deskaudio_probe
