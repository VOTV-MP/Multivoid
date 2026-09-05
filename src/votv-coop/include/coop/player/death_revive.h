// coop/player/death_revive.h -- the player feels the whole native death and keeps the world.
// Nothing about the death is suppressed: the sound plays, the dead flag sets, the delays run,
// the black screen lands, the full ten seconds. The cut is at the last stage, the level-open
// call where the game reaches for the level travel: that one call is refused and a revive
// written in its place. The decision and the work are split on purpose: the veto is data
// only and runs in line inside the level-open detour (a raw dead byte, an atomic session
// check, a liveness check, an arm flag), with zero engine dispatch, since a UFunction call
// from that frame re-enters our ProcessEvent detour and fires the interceptors inside the
// vetoed frame; the revive is deferred to the next pump task. The arm decision is made at
// the death edge, not at the travel: it is the same decision as whether the pump flees,
// which keeps the two from disagreeing. Session-gated on the session running (a host with
// zero clients is a session; single player has none, so vanilla is untouched); the local
// pawn's dead flag is the discriminator, fail-closed (a dead player cannot open the pause
// menu, and our own flees stop the session before travelling, so they pass through). If the
// revive fails, we leave: every step is verified by read-back, and any shortfall flees.

#pragma once

namespace coop::net { class Session; }

namespace coop::death_revive {

// Install the travel seam and publish the veto. Idempotent, cheap after the first call; safe
// to call every pump tick, it latches. `session` is cached for the veto's session test.
void Install(coop::net::Session* session);

// The pump barrier. Publishes the veto's inputs from validated state, arms on the dead
// flag's rising edge, and runs a pending revive. `localPawn` is the local player, already
// liveness-checked by the caller this tick (null parks the module). Game thread only.
void Tick(coop::net::Session& session, void* localPawn);

// The unconditional watchdog, driven from the harness's timeline tick, not from the pump. If
// a travel was refused and no revive has run within its deadline, this leaves the world
// rather than strand a dead player who cannot open the pause menu. It must not live behind
// any of the gates the revive itself depends on: covering a failure of the pump is its whole
// job. Safe off the game thread (it posts the flee).
void Watchdog();

// Reset every latch for a new session.
void OnSessionStart();

// Is this death being answered by the revive? The pump's death edge asks this to decide
// whether to flee. True means the arc owns this death and the flee must not pre-empt it;
// false means the seam is unavailable (a stale signature, unresolved verbs, no session) and
// the caller's flee is the fallback that keeps the death survivable without us.
bool ArmedForThisDeath();

// Diagnostics for the acceptance instrument: has the seam ever refused a travel, and did the
// last revive complete its conjunction?
bool SeamInstalled();
unsigned long long TravelsRefused();
bool LastReviveSucceeded();

// The negative control, armed by VOTVCOOP_DEATH_NO_RECONCILE=1. When true the reconcile does
// nothing, so the death's un-disposed writes are left standing. It exists for one reason:
// the write-diff instrument (coop/dev/death_write_diff.h) measures which writes the death
// leaves behind, and with the reconcile enabled our own fix hides the two it most needs to
// re-find; an instrument that can only run in the arm where the defect is already patched
// grades itself green. It gates the reconcile only: the travel is still refused and the
// player still revived, so a run with it set is survivable but dirty, never stranding
// anyone. Read once and cached; a drill switch, not a shipped configuration. Dirty is not
// cosmetic: skipping the reconcile leaves the game instance's death signal armed, on an
// object that outlives every level and whose branch reaches the game's end verb while
// paused, so this arm plus a later Escape is a plausible session-ender. The drill never
// pauses, so nothing is broken today; do not reach for this switch outside the drill.
bool ReconcileDisabled();

}  // namespace coop::death_revive
