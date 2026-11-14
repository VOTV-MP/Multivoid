// coop/dev/cover_probe.h -- [dev] the covers on the rig (VOTVCOOP_COVER_PROBE=1), for Ghezra/Multivoid issue 30: a
// client unbolts the signal desk's back plate, takes it off, sets it back and bolts it, then opens and shuts a
// transformer station's door; the host then unbolts another plate and opens the door; both log the desk and the door.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::cover_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::cover_probe
