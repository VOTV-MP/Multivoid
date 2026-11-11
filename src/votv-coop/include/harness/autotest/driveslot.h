// harness/autotest/driveslot.h -- the drive-slot test's entry points, in their own header for the
// reason slip_drill.h gives: a drill with a doc of its own is declared beside its implementation.

#pragma once

#include <windows.h>

namespace harness::autotest {

// Two peers, one run. The host births a drive, seeds a recognisable signal into its data_0 and seats it in the desk's
// play slot; the client takes it out the way a player does, and each peer judges its own copy and says one
// "driveslot: VERDICT <host|client> <arm> PASS|FAIL" line, "driveslot: done" ending the phase. The env value
// VOTVCOOP_RUN_DRIVESLOT, on both peers, picks the arm:
//   grab    (or 1) playerTryToGrab + a physics-handle carry: the same actor ends off the port, unfrozen, on both peers;
//   hold    playerTryToHold -> driveAction -> Add Object to Free Hotbar Slot, through the act lane: the client's main
//           hand holds the drive with its data, the host's puppet shows the mirror, no world drive is left;
//   pocket  playerTryToCollect -> driveAction -> addObjectToInventory, through the act lane: the client's bag holds
//           the drive's record with its data, no world drive is left.
// See autotest_driveslot.cpp.
void RunDriveSlotTest();
DWORD WINAPI DriveSlotTestThread(LPVOID arg);

// Whether VOTVCOOP_RUN_DRIVESLOT names an arm (1, grab, hold or pocket): the dispatcher spawns the test on it.
bool DriveSlotArmed();

}  // namespace harness::autotest
