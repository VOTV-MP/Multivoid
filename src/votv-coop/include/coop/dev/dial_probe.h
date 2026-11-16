// coop/dev/dial_probe.h -- [dev] the signal desk's frequency and polarity dials turned by the clients, by wheel
// notches, logged on every peer (VOTVCOOP_DIAL_PROBE=1, VOTVCOOP_DIAL_SCRIPT=a|b on a client), for Ghezra/Multivoid
// issue 32: each peer's offsets fifty times a second and its DeskInput counts at the end.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::dial_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::dial_probe
