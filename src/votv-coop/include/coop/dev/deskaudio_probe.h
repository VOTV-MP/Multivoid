// coop/dev/deskaudio_probe.h -- [dev] whether the signal desk's sound components are heard in space
// (row deskaudio_probe, env VOTVCOOP_DESKAUDIO_PROBE), for Ghezra/Multivoid issue 29: each component's spatialization
// and attenuation, and its sound's, logged once, then `rig: VERDICT deskaudio PASS|FAIL|INVALID` (PASS: the cursor loop
// holds the ping loop's attenuation settings; FAIL: none or others). Red: deskaudio_att_off.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::deskaudio_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

// The session ended: the next one is judged afresh.
void OnDisconnect();

}  // namespace coop::dev::deskaudio_probe
