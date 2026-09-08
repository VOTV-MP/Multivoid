// coop/pause_guard.h -- the coop no-pause invariant (ONE owner, both roles).
//
// USER REPORT (2026-07-04): a client pressing ESC in a session pauses ITS world --
// the engine stops ticking, its pose stream freezes on every other screen, "clients
// freeze". SP semantics: ESC (mainPlayer InpActEvt_Escape -> the mainGamemode
// ubergraph, stmts [372]/[375]/[3035]) opens pause_mainMenu (ui_menu_C::enterPause,
// isPause@0x4C0) and pauses the world via GameplayStatics::SetGamePaused -- dispatched
// as EX_CallMath, PE-INVISIBLE (docs/coop-dispatch-visibility.md), so no ProcessEvent
// interceptor can cancel it; and the console `pause` command reaches the same engine
// state through APlayerController::SetPause without touching GameplayStatics at all.
//
// MP semantics (MTA precedent, reference/mtasa-blue: no world pause exists anywhere in
// a session): while a coop session is CONNECTED the world NEVER stops ticking, on any
// peer -- host or client. The ESC menu itself stays fully usable: it is just a widget;
// only the world-pause side effect is removed.
//
// Mechanism: enforce the STATE, not the call sites (anti-smear: one owner for the
// "world pause" axis). Every gameplay tick while connected: if the world reads paused
// (engine::IsGamePaused) -> engine::SetGamePaused(false), the game's own un-pause
// verb. Level-triggered = catches EVERY pause source (ESC, console pause, any future
// path) at one seam. Poll LIVENESS while paused is guaranteed by the pump's
// ProcessEvent ride: the pause menu widget's own Tick (ui_menu_C -- the exact tickFn
// multiplayer_menu latches) dispatches ProcessEvent every Slate frame even while the
// world is paused, so the un-pause lands within a frame of the pause engaging.
//
// Solo (no session) is untouched -- SP pause behaves natively. Game-thread only.

#pragma once

namespace coop::pause_guard {

// Per-gameplay-tick enforcement (subsystems::TickGameplay chain, so it only runs
// world-up). `connected` = Session::connected() -- the invariant's scope. Cheap when
// unpaused (one reflected IsGamePaused read); logs once per pause episode.
void Tick(bool connected);

}  // namespace coop::pause_guard
