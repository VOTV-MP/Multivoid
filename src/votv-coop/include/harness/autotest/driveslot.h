// harness/autotest/driveslot.h -- the drive-slot test's entry points, in their own header for the
// reason slip_drill.h gives: a drill with a doc of its own is declared beside its implementation.

#pragma once

#include <windows.h>

namespace harness::autotest {

// Two peers, one run. The host puts a fresh drive in the desk's play slot; the client takes it out
// the way a player's grab does and carries it aside. The host's copy must end out of the slot,
// unfrozen and carried off the port: its "driveslot: VERDICT host PASS|FAIL" line is the result,
// and "driveslot: done" ends the phase. Env VOTVCOOP_RUN_DRIVESLOT=1, on both peers.
void RunDriveSlotTest();
DWORD WINAPI DriveSlotTestThread(LPVOID arg);

}  // namespace harness::autotest
