// harness/harness.h -- autonomous test harness, ported into the standalone mod.
//
// This is the C++ port of tools/probes/coopTestHarness (a UE4SS Lua mod). It
// runs INSIDE the standalone mod (RULE No.3 -- no UE4SS), driving the engine
// through ue_wrap (game-thread dispatcher + ExecuteConsoleCommand): skip the
// menus into gameplay, screenshot, and report state, with no manual clicking.
// The Lua harness is retired once this reaches parity (RULE No.2).
//
// Not engine-wrapper and not coop/network logic -- it is dev tooling, kept in
// its own subtree so it is trivially separable.

#pragma once

namespace harness {

// Start the harness: read the scenario, then run its timeline on a background
// thread (each engine action is posted to the game thread). Requires the
// game-thread dispatcher to be installed first. Non-blocking.
//
// Scenario is read from `scenario.txt` next to the mod DLL (one word):
//   newgame -> skip straight into gameplay (open untitled_1), then report/shot
//   none    -> launch only; do nothing automatic
// Missing file defaults to "newgame" (the autonomous-test default).
void Start();

}  // namespace harness
