// coop/dev/atv_riders_drill.h -- [dev] the ATV riders drill: a client's body seated in an ATV and the host's kerfur riding it,
// shown on every peer, each step decided by an event (coop/interactables/atv_drill_events.h), never by a clock.
// One drill thread per peer, armed by a value of the `atv_riders_drill` row on EVERY peer: `on` is the green run and
// `drive_control` is the green run with the host's kerfur kept off the ATV in step 7, so the host's own drive is measured
// without it; each other value is a red, read only by the module whose mechanism it turns off. Three peers: the host, the observer in
// slot 1 (mp.py's first client, which leaves and rejoins) and the driver in slot 2 (mp.py --observer's client). The peers order
// their steps through harness/autotest/peer_signal and move only with the bot director. The rejoined observer's life is told
// by VOTVCOOP_ATV_RIDERS_REJOIN=1 (mp.py --rejoin-env). A verdict is said the moment it is decided, on
// `rig: VERDICT atv-riders PASS|FAIL|INVALID`; a FAIL or an INVALID ends every peer's sequence at once, and a time bound can
// only say INVALID. Each step says `atv-riders: step <n> PASS`.
#pragma once

#include <windows.h>

namespace coop::dev::atv_riders_drill {

// The drill thread for the harness dispatch (CreateThread): host, observer or driver by the role and the slot.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::atv_riders_drill
