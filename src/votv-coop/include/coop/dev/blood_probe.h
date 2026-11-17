// coop/dev/blood_probe.h -- [dev] a player's blood on the ground (VOTVCOOP_BLOOD_PROBE=client|host on both peers): the
// bleeder lets drops fall through the blood master; both peers count their blood marks.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::blood_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::blood_probe
