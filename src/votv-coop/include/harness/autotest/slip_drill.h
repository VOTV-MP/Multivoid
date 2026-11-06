// harness/autotest/slip_drill.h -- the slip drill's entry points.
//
// Its own header rather than a row in harness/autotest/drill_threads.h, which holds one line per
// older drill. A drill with a doc of its own is declared beside its module
// (death_state_probe.h is the same shape).

#pragma once

#include <windows.h>

namespace harness::autotest {

// Solo host, or a client linked to one (`mp.py slip --client`). A banana peel slips the local
// player twice -- once with the pawn's `dead` false, the control, which must only ragdoll, and
// once with it true -- to settle whether a zero-damage ragdoll re-enters the death chain, and
// whether our run-ending seam refuses the menu travel that chain asks for. It needs a live
// session either way. Env VOTVCOOP_RUN_SLIP_DRILL=1; lines tagged [SLIP].
void RunSlipDrill();
DWORD WINAPI SlipDrillThread(LPVOID arg);

}  // namespace harness::autotest
