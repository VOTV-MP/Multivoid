// harness/autotest.h -- the autotest island's interface. Each autonomous routine is a Run* and
// *Thread pair declared in harness/autotest/drill_threads.h and defined in its own
// harness/autotest/autotest_*.cpp (dispatch table: autotest_dispatch.cpp). A routine is gated by
// its VOTVCOOP_RUN_* env var, spawned from the play loop on its own thread, and does all engine
// work through GT::Post, never touching UObject state off the game thread.

#pragma once

#include <windows.h>

#include "harness/autotest/drill_threads.h"   // every routine's Run*/*Thread pair
#include "harness/autotest_trash.h"           // the trash carry drills

namespace harness::autotest {

// The one role predicate for every routine's host/client branch: the registry's resolve of
// net.role (env twin, ini, unset) through coop::config::ResolveEnum, the product's own resolver.
// "client" is the client; anything else is the host side. Latched: one resolve per process,
// since routines poll from worker threads.
bool IsClientRole();

// A host routine's readiness wait: slot `slot` is seated and its world is ready, the peer's
// own milestone (what its rig: READY peer-world-ready line reports), polled every 50 ms for at most
// `budgetMs`. False when the budget ran out; the caller names its phase. Any thread.
bool WaitPeerWorldReady(int slot, DWORD budgetMs);

}  // namespace harness::autotest
