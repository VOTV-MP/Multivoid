// coop/dev/ufo_probe.h -- [dev] the ambient UFOs on the rig (VOTVCOOP_UFO_PROBE=1), for coop/world/ufo_sync.
//
// HOST, 20 s after a client is connected: the gamemode's own ufo_ball until it mints a ball, then a boo, a pill and
// a Joel minted near the host, then the Midas UFO's take-off. CLIENT, 20 s after its ghost sweep: each of its own five
// UFO timers, which the spawn authority refuses. BOTH, every 2 s: each live UFO's element id and where its moving
// part is (`[UFO] at`), so the host's and the client's lines can be paired.
#pragma once

namespace coop::net { class Session; }

namespace coop::dev::ufo_probe {

// Per frame. A latched read when the probe is off.
void Tick(coop::net::Session* session);

}  // namespace coop::dev::ufo_probe
