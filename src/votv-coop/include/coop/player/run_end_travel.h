// coop/player/run_end_travel.h -- the seam that decides which trip to the main menu is the game
// ending the run, and refuses the ones nobody asked for.

// VOTV ends a run by travelling to the "menu" level, and dying is only one of the ways it gets
// there: a whole-cook census puts ONE caller under `UGameplayStatics::OpenLevel`
// (`mainGamemode_C::transition`), ONE under that (`lib_C::loadLevel`), and 25 call sites under
// THAT, nine of them bound for "menu" by a literal. Only the death chain sets `dead`, so an arm
// keyed on that flag let five in-world run-endings through and our layer ended the session for
// every peer. Nine and five are FLOORS: eight more sites pass a runtime level name, one of them a
// level-placed instance variable no cook census can read -- which is why the seam judges the name
// it is actually called with. Public account: `docs/players.md`; the verdicts are in the .cpp.

// Every one of those 26 sites is `EX_LocalVirtualFunction`, invisible to a ProcessEvent detour:
// the seam is a watch on `ue_wrap/core/script_gate`, the detour on the VM's own body loop, which
// sees the call with its arguments and can refuse it. Gameplay layer (principle 7) -- it reaches
// the engine only through the gate and reflection.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::player::run_end_travel {

// Register the watch on `lib_C::loadLevel` and cache the session the verdict tests. Idempotent;
// safe every tick. The watch is registered whatever the role: both peers end their own runs.
void Install(coop::net::Session* session);

// Publish the seam's readiness to the revive's arm while a session runs; the name resolve is
// Install's. Game thread.
void Tick();

// Reset the per-session latches and counters.
void OnSessionStart();

// The cancelled travel is handed to `death_revive` by a push, not a poll: this lane knows the
// author and the instant, the revive owns the pending state it already had, and one direction of
// include keeps the two modules acyclic. `death_revive` never names this header.

// Why a menu-bound travel was allowed, or that it was not. One enum so the verdict has a name in
// a log line and in a drill instead of a boolean nobody can attribute. Named Judgement rather
// than Verdict because the gate's own `script_gate::Verdict` sits beside it at every call site.
enum class Judgement : uint8_t {
    RunNoSession,    // solo, or a session that is not running: never our business
    RunPlayerAsked,  // the author is the pause menu: they asked to leave
    RunNoRevive,     // we cannot answer it, so we must not refuse it
    Cancel,          // the game is ending the run and nobody asked
};

// The same classification the seam runs, with no side effect, for the drill: it must be possible
// to assert BOTH directions of the discriminator without actually travelling to the menu, and a
// second copy of the decision in the test would be the thing most likely to drift. `author` is
// what a `loadLevel("menu", ...)` call would pass as `__WorldContext`. Game thread.
Judgement JudgeMenuTravel(void* author);

// Diagnostics for the acceptance instrument. `seen` counts every `loadLevel` the gate reached,
// `menuSeen` those bound for the menu, `cancelled` those refused: a quiet seam and an absent one
// read the same in `cancelled` alone, which is what made the field log ambiguous in the first
// place.
bool WatchInstalled();
unsigned long long TravelsSeen();
unsigned long long MenuTravelsSeen();
unsigned long long TravelsCancelled();

}  // namespace coop::player::run_end_travel
