// coop/player/death_revive.h -- the player feels the whole native run-ending and keeps the world.
// Nothing is suppressed: the sound plays, the dead flag sets, the delays run, the black screen
// lands, the full ten seconds. The cut is the travel the ending asks for, and WHICH travels are a
// run ending is `coop/player/run_end_travel`'s question, not this module's. The two used to be
// one thing here, armed on the local pawn's `dead` flag -- which exactly one of the game's six
// routes to the menu sets, so the other five ended the session for every peer. This module now
// owns only the answer: what a player comes back as.

// The decision and the work stay split. The seam cancels in line, inside the VM's body loop,
// doing nothing an engine dispatch would have to re-enter; the revive is deferred to the next
// pump task. The arm is still read at the `dead` edge rather than at the travel, because it is
// the same decision as whether the pump flees and the two must not disagree. Session-gated on
// the session running (a host with zero clients is a session; single player has none). If the
// revive fails we leave: every step is verified by read-back, and any shortfall flees.

#pragma once

namespace coop::net { class Session; }

namespace coop::death_revive {

// Cache the session the arm tests. Idempotent and cheap; safe to call every tick, from any
// thread, and safe before the world exists.
void Install(coop::net::Session* session);

// The pump barrier. Arms on the dead flag's rising edge and runs a revive the seam asked for.
// `localPawn` is the local player, already liveness-checked by the caller this tick (null parks
// the module). Game thread only.
void Tick(coop::net::Session& session, void* localPawn);

// Can a run ending be answered right now? The seam asks before it cancels anything: refusing a
// travel we cannot answer is the one outcome worse than the menu. True means the revive's verbs
// are resolved, the dead flag's offset is known and a session is live. Any thread.
bool ReviveAvailable();

// The seam publishes its own readiness here, so this module never has to name it back; the arm
// at the death edge is `ReviveAvailable() && the seam can cancel`. Any thread.
void NoteRunEndSeamReady(bool ready);

// The seam cancelled a run-ending travel: the world is kept and this module owes a revive on the
// next pump task. `authorClass` is the blueprint that asked to travel -- the local `mainPlayer_C`
// for the death chain, an ending's own class otherwise -- and is carried only to name the ending
// in the revive's line. Game thread (the gate's callback contract).
void NoteRunEndCancelled(void* author, const wchar_t* authorClass, bool authorIsLocalPawn);

// A run-ending travel a peer's puppet authored here was refused: this machine's player did not die
// and is not revived, but the copy's death chain may have put a black screen on this viewport, which
// is cleared over the cleanup window. Any thread.
void NotePeerPuppetRunEndCancelled();

// The unconditional watchdog, driven from the thread that POSTS the pump composite rather than
// from inside it -- a lens found it riding in the composite, where a stalled game thread stops
// the watchdog and the task it watches together. If a travel was cancelled and no revive has run
// within its deadline, this leaves the world
// rather than strand a dead player who cannot open the pause menu. It must not live behind
// any of the gates the revive itself depends on: covering a failure of the pump is its whole
// job. Safe off the game thread (it posts the flee).
void Watchdog();

// Reset every latch for a new session.
void OnSessionStart();

// Is this death being answered by the revive? The pump's death edge asks this to decide
// whether to flee. True means the arc owns this death and the flee must not pre-empt it;
// false means the seam is unavailable (an unregistered watch, unresolved verbs, no session) and
// the caller's flee is the fallback that keeps the death survivable without us.
bool ArmedForThisDeath();

// Did the last revive complete its conjunction? The travel counters live with the seam that
// sees them (`coop/player/run_end_travel`).
bool LastReviveSucceeded();

// How long after the local pawn first ticked the arm became answerable, or -1 while it still is
// not. The arm's terms are all readiness terms, and a client reaches its own pawn later than a
// host does -- so a death inside that window is not armed, and the pump's flee ends the session
// for everyone. This is that window, measured: the number now rides every log, and a drill reads
// it to tell the steady-state death it means to test from a race it does not. Reset per session,
// from the session's own thread rather than the game thread, which is why the window's fields are
// atomic. Any thread.
long long ArmReadyAfterPawnMs();

// The negative control, armed by VOTVCOOP_DEATH_NO_RECONCILE=1: the reconcile does nothing, so
// the ending's un-disposed writes are left standing. It exists for the write-diff instrument
// (coop/dev/death_write_diff.h), which measures what the ending leaves behind and would grade
// itself green in the only arm where the defect is already patched. It gates the reconcile
// alone: the travel is still cancelled and the player still revived, so a run with it set is
// survivable but dirty. A drill switch, read once and cached, not a shipped configuration.
// Its subject shrank when the cut moved: `lib_C::loadLevel`'s own writes are no longer made at
// all, so what is left to suppress is the damage HUD's full-screen red.
bool ReconcileDisabled();

}  // namespace coop::death_revive
