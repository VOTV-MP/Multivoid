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
// The scenario comes from the VOTVCOOP_SCENARIO env var (per-launch signal;
// the on-disk scenario.txt fallback was RETIRED 2026-06-06 because a leftover
// file aliased later native launches -- see coop::config::ReadScenario).
// No env (a native launch) -> "menu": boot to VOTV's own main menu.
void Start();

}  // namespace harness
