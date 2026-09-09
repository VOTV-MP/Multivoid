// coop/session/pause_guard.h -- the coop no-pause invariant (ONE owner, both roles).
//
// A client pressing ESC in a session pauses ITS world: the engine stops ticking and its pose
// stream freezes on every other screen. SP semantics: ESC opens pause_mainMenu and pauses the
// world with GameplayStatics::SetGamePaused, dispatched as EX_CallMath and so PE-INVISIBLE
// (docs/coop-dispatch-visibility.md) that no ProcessEvent interceptor can cancel it, while the
// console `pause` reaches the same state through APlayerController::SetPause. On the MTA
// precedent a connected session has no world pause at all: it never stops ticking on any peer,
// and the ESC menu stays usable, minus that side effect.
//
// So the STATE is enforced rather than the call sites -- one owner for the "world pause" axis.
// Every gameplay tick while connected, a world that reads paused (engine::IsGamePaused) is
// un-paused with the game's own verb; level-triggered, so every pause source is caught at one
// seam. The poll stays live while paused because the pause menu widget's own Tick dispatches
// ProcessEvent every Slate frame. Solo is untouched. Game thread only.

#pragma once

namespace coop::pause_guard {

// Per-gameplay-tick enforcement (subsystems::TickGameplay chain, so it only runs
// world-up). `connected` = Session::connected() -- the invariant's scope. Cheap when
// unpaused -- one reflected IsGamePaused call; logs once per pause episode.
void Tick(bool connected);

}  // namespace coop::pause_guard
