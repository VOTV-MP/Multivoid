// harness/autotest_trash.h -- the drills of the trash carry: the host's own grab, a client's
// grab through its intent, the host's throw watched by a client, and the census of how every pile
// and clump looks on both peers. Declared apart from harness/autotest.h, which includes this.

#pragma once

#include <windows.h>

namespace harness::autotest {

// The chipPile grab test, host-driven: the host is teleported to a pile, aimed until the game's
// own trace names it, and InpActEvt_use fired through the same edge a real press hits;
// holdingActor_main is measured and a throw tests the re-pile. It produces the log to read, never a
// pass by itself. Env VOTVCOOP_RUN_CHIPPILE_TEST=1.
void RunAutonomousChipPileTest();
DWORD WINAPI ChipPileTestThread(LPVOID arg);

// The synthetic GrabIntent test: the client faces a mirrored pile, presses use, carries and
// releases; the host validates, grabs on the puppet, streams the carry and lands it. Env
// VOTVCOOP_RUN_GRAB_INTENT_TEST=1.
void RunGrabIntentTest();
DWORD WINAPI GrabIntentTestThread(LPVOID arg);

// The host-throw scenario (VOTVCOOP_RUN_HOSTTHROW=1): the host walks to a pile with the bot
// director, grabs and throws it; the client samples its own mirror of the clump.
void RunHostThrowScenario();
DWORD WINAPI HostThrowThread(LPVOID arg);

// The pile-look census (VOTVCOOP_RUN_PILELOOK=1): each peer logs every chip pile it can name by
// eid with its visible mesh rotation; the driver joins the two logs. Read-only.
void RunPileLookScenario();
DWORD WINAPI PileLookThread(LPVOID arg);

}  // namespace harness::autotest
