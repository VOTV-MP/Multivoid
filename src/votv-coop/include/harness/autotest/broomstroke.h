// harness/autotest/broomstroke.h -- the broom-stroke drill's own declaration.
//
// Beside its implementation rather than in harness/autotest/drill_threads.h, the one-line
// catalogue of the older drills. A drill is one feature; this is where its interface belongs.
//
// It measures a whole broom stroke, swung through the broom's own right mouse button on a broom each
// peer really holds, followed on both peers by element id: each peer holding the button, the host
// sweeping a heap and the client a lone pile, the client emptying a dispenser, each peer pushing the
// trash, and on the heap's floor two clumps held back from re-piling, one knocked up and one pushed
// again. A two-peer driver joins the two logs. Env VOTVCOOP_RUN_BROOM_STROKE=1, on both peers.

#pragma once

#include <windows.h>

namespace harness::autotest {

void RunBroomStrokeProbe();
DWORD WINAPI BroomStrokeProbeThread(LPVOID arg);

}  // namespace harness::autotest
