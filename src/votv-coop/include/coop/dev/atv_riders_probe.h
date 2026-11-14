// coop/dev/atv_riders_probe.h -- [dev] the ATV riders probe: four measurements of how a remote ATV rider shows, taken in one
// three-peer run (the host, the client in slot 1 that drives, the observer in slot 2; mp.py --observer), armed by
// `atv_riders_probe=on` on every peer. Observe only, except that the driver's body is attached to the ATV in one call in place
// of the per-frame seat (coop/player/puppet_seat). Every line is `atv-probe: <what>`:
//   M1 sitOnCar POST / getOffCar PRE   the host's kerfur's own calls, seen by the script gate
//   M2 body / kerfur / summary          the attached body and the riding kerfur mirror against the seat, through a 6 m drive
//   M3 mirror / tryToOccupyCar          the driving client's kerfur mirrors at its own sit, and the call the sit makes on them
//   M15 skin                            the seat pose (isOnAtv) of the driver's body across a re-skin
// A phase ends on an event: a table seat, a signal of another peer, the host's own copy moved 6 m. A bound on a wait ends the
// probe on `atv-probe: INVALID <phase> <why>`; the run ends on `atv-probe: DONE`. Moves only with the bot director. Game thread
// for the engine; the probe's thread hops there.
#pragma once

#include <windows.h>

namespace coop::dev::atv_riders_probe {

// The probe thread for the harness dispatch (CreateThread): host, client or observer by the role and the slot.
DWORD WINAPI Thread(LPVOID arg);

}  // namespace coop::dev::atv_riders_probe
