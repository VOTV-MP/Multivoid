// coop/player/death_revive.h -- the player feels the WHOLE native death, and keeps the world.
//
// USER, 2026-08-31, verbatim: *"Я хочу чтобы игрок ощущал нативную смерть, но без выкидывания
// в главное меню... момент когда игра захочет отослать игрока в главное меню и выгрузить карту
// и сломать весь мир - тут наш мод вступает и НОВОЕ ИЗМЕНЕННОЕ СОСТОЯНИЕ ПИШЕТ."*
//
// So NOTHING about the death is suppressed. The sound plays, `dead := true`, the two
// RetriggerableDelays run, the black screen lands at +5 s -- the full ten seconds. The cut is
// at the LAST stage: `UGameplayStatics::OpenLevel`, where the game reaches for the level
// travel. We refuse that one call and write a revive in its place.
//
// Design of record: `docs/DEATH_ARC.md`. Read it before touching anything here. What follows
// is only what a reader of THIS file needs.
//
// ---- THE SHAPE, AND WHY IT IS SPLIT IN TWO -----------------------------------------------
//
// The decision and the work happen in different places, on purpose:
//
//   * THE VETO is DATA ONLY and runs IN LINE, inside the OpenLevel detour: a raw `dead` byte
//     read at a cached offset, an atomic session check, a GUObjectArray liveness check, and an
//     atomic arm flag. ZERO engine dispatch (level_travel.h spells out why: a UFunction call
//     from that frame re-enters our ProcessEvent detour and fires ~20 interceptors -- one
//     returning true would SKIP the verb we just called -- inside the vetoed frame).
//   * THE REVIVE is DEFERRED to the next pump task, where every other engine call this mod
//     makes already happens and where a BP dispatch is ordinary.
//
// An earlier pass converged on doing both in line and was REVERSED: the distinction I had
// offered between that frame and the measured `[[lesson-vm-bracket-zero-engine-mid-verb]]`
// corruption was structural reasoning, not a measurement, and a safe alternative existed.
//
// ---- WHY THE ARM DECISION IS MADE AT THE DEATH EDGE, NOT AT THE TRAVEL --------------------
//
// `Arm()` happens in the pump on the `dead` RISING EDGE -- ten seconds before the travel --
// and it is the SAME decision as "should net_pump flee?". That is not an optimisation, it is
// what keeps the two from disagreeing. If the veto decided at OpenLevel, a death whose flee
// had already been skipped could still find itself unable to revive, and the world would be
// torn down with our coop state in it -- the H1 shape the retired KO lane shipped (a safety
// released only on a path that assumes the happy case ran). One decision, one moment, both
// consequences.
//
// ---- WHAT BOUNDS IT ----------------------------------------------------------------------
//
// * SESSION-GATED, and the user defined the term (2026-08-31): *"Solo host in a session a a
//   coop session and revive should work there. Single player is when playing solo game in solo
//   save, no session."* So the gate is `Session::running()` and nothing more -- a HOST with
//   zero clients IS a coop session; single-player has no session, so the veto's first term is
//   false and vanilla VOTV is untouched. The detour is installed process-wide (it has to be --
//   it is a function detour), which is exactly why the session test lives in the VETO.
// * `dead == true` on the local pawn is the discriminator, fail-CLOSED. `[V]` a dead player
//   cannot open the pause menu at all (mainPlayer uber @21777 ORs `dead` into the deny), so no
//   player-authored quit can ever travel with it set; and our own flees call `Session::Stop()`
//   BEFORE travelling (`net_pump.cpp` FleeToMainMenu), so a flee's travel arrives with
//   `running() == false` and passes straight through. The live-session term SUBSUMES an
//   authorship bracket -- do not build one.
// * IF THE REVIVE FAILS, WE LEAVE. Every step is verified by read-back and the conjunction is
//   checked before the revive is called done; anything short of it calls net_pump's flee,
//   whose `Stop()` disarms this veto so the next travel proceeds. There is no state in which
//   the player is held in a world we refused to leave and cannot fix.

#pragma once

namespace coop::net { class Session; }

namespace coop::death_revive {

// Install the travel seam and publish the veto. Idempotent, cheap after the first call.
// Safe to call every pump tick; it latches. `session` is cached for the veto's session test.
void Install(coop::net::Session* session);

// The pump barrier. Publishes the veto's inputs from validated state, arms on the `dead`
// rising edge, and RUNS a pending revive. `localPawn` is the local `mainPlayer_C`, already
// liveness-checked by the caller this tick (may be null -- the module then parks).
// Game thread only.
void Tick(coop::net::Session& session, void* localPawn);

// The unconditional watchdog, driven from the harness's timeline tick (NOT from the pump).
// If a travel was refused and no revive has run within its deadline, this leaves the world
// rather than strand a dead player who cannot open the pause menu. It must not live behind any
// of the gates the revive itself depends on -- covering a failure of the pump is its whole job.
// Safe off the game thread (it posts the flee).
void Watchdog();

// Reset every latch for a new session.
void OnSessionStart();

// "Is this death being answered by the revive?" -- net_pump's death edge asks this to decide
// whether to flee. TRUE means the arc owns this death and the flee must NOT pre-empt it;
// FALSE means the seam is unavailable (stale signature, unresolved verbs, no session) and the
// caller's flee is the fallback that keeps the death survivable without us.
bool ArmedForThisDeath();

// Diagnostics for the acceptance instrument: has the seam ever refused a travel, and did the
// last revive complete its conjunction?
bool SeamInstalled();
unsigned long long TravelsRefused();
bool LastReviveSucceeded();

// THE NEGATIVE CONTROL, armed by `VOTVCOOP_DEATH_NO_RECONCILE=1`.
//
// When true, `ReconcileCancelledTravel()` does nothing, so the death's un-disposed writes are
// left standing. This exists for ONE reason: the write-diff instrument
// (coop/dev/death_write_diff.h) measures which writes the death leaves behind, and with the
// reconcile ENABLED our own fix hides the two it most needs to re-find --
// `[[lesson-an-instrument-that-shares-the-defect-cancels-it]]`. An instrument that can only
// ever be run in the arm where the defect is already patched grades itself green.
//
// It gates the RECONCILE ONLY. The travel is still refused and the player is still revived,
// so a run with this set is survivable-but-dirty, never a run that strands anyone. It is read
// once and cached; it is a drill switch, not a shipped configuration.
//
// "Dirty" is not cosmetic, and a post-ship audit was right to say so: skipping the reconcile
// leaves `gameInstance.NewVar_1` ARMED, and that death signal lives on an object which
// outlives every level and whose branch reaches `lib_C::end(self)` WHILE THE GAME IS PAUSED.
// So this arm plus a later ESC is a plausible session-ender. The drill never pauses, so
// nothing is broken today -- but do not reach for this switch outside the drill.
bool ReconcileDisabled();

}  // namespace coop::death_revive
