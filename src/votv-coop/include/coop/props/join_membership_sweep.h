// coop/props/join_membership_sweep.h -- the join-window world-membership claim + divergence sweep.
//
// ONE concept: at join time, TRACK which world members the host expressed in the snapshot bracket (the claim
// set), then at load-tail quiescence SWEEP away the locals the host's snapshot did NOT claim (the client's
// save-loaded world minus the host's = the divergence). This is MTA's deletion-by-tracked-membership
// (CElementGroup): we only ever adjudicate what THIS client loaded, never a GUObjectArray scan of the world.
//
// EXTRACTED from coop/props/remote_prop_spawn.cpp 2026-06-30 (anti-smear: that file held TWO concepts -- the
// wire PropSpawn RECEIVER (OnSpawn, which stays) AND this membership-claim-and-sweep -- and was over the 1500
// LOC hard cap). The cut is clean: the only seam is RecordClaimIfTracking (OnSpawn records each bound actor
// into the claim set) + IsClaimTrackingActive (OnSpawn gates a level-pile twin-destroy on an open bracket).
// [[feedback-folder-per-domain-concept-rule]]
//
// LIFECYCLE (all game-thread only -- the event_feed drain + the client reconcile tick + the net_pump
// disconnect edge; no mutex):
//   BeginClaimTracking()  -- SnapshotBegin: arm a fresh bracket (clear the claim set, cancel a stale sweep).
//   RecordClaimIfTracking(actor) -- each PropSpawn / self-announce binds an actor -> it survives the sweep.
//   ArmDivergenceSweep()  -- SnapshotComplete: defer the sweep to load-tail quiescence (NOT inline).
//   TickClientReconcile() -- every client tick: drives quiescence_drain::OnTick, then (when a sweep pends +
//                            the load tail has quiesced) fires the one-shot RunDivergenceSweep_.
//   OnClientWorldReadyResetSweep() -- a within-session world change cancels a pending sweep.
//   ResetClaimTracking()  -- session teardown: clear the claim set + cancel the sweep + Reset the downstream
//                            join modules (pile_spawn_bind / quiescence_drain / kerfur_reconcile / mirror_defer).
//
// The sweep CALLS the downstream join modules (it is their driver, not their owner): pile_spawn_bind (the pile
// spawn-bind index Reset), quiescence_drain (the deferred-reconcile sequence + OnTick + the teardown Reset).

#pragma once

#include <unordered_set>

namespace coop::join_membership_sweep {

// The claim set (read-only). remote_prop_spawn::OnSpawn passes it to pile_spawn_bind::TryDestroyTwin /
// FindAndConsumeAdoptCandidate so an already-claimed native is skipped. Game thread.
const std::unordered_set<void*>& ClaimedActors();

// SnapshotBegin: arm a fresh claim bracket (clears the prior claim set + cancels a stale pending sweep +
// resets the pile spawn-bind index + the completeness census + the spawn-order probe). Game thread.
void BeginClaimTracking();

// Record that the host snapshot (or a self-announce) bound `actor` this bracket -- it is accounted-for by the
// host's world and must survive the sweep. No-op when tracking is disarmed (a live PropSpawn outside a join
// costs one bool read). Called from remote_prop_spawn::OnSpawn + the self-announce sites. Game thread.
void RecordClaimIfTracking(void* actor);

// True while a snapshot bracket is open (BeginClaimTracking -> sweep/reset). remote_prop_spawn::OnSpawn reads
// it to gate the level-pile twin-destroy to the join window. Game thread.
bool IsClaimTrackingActive();

// SnapshotComplete: defer the one real divergence sweep to load-tail quiescence (it must adjudicate the FULLY
// reloaded world, not a mid-load snapshot). Claim tracking stays armed through the quiesce window. Game thread.
void ArmDivergenceSweep();

// Every client reconcile tick: drives quiescence_drain::OnTick (the steady-state identity reconcile, bracket-
// independent), then -- only while a sweep pends -- polls the load-tail quiescence probe and fires the one-shot
// membership sweep once the async load has drained (or a deadline trips). Game thread.
void TickClientReconcile();

// A within-session world change (cave/level travel) cancels a sweep armed for the OLD world. Game thread.
void OnClientWorldReadyResetSweep();

// The membership predicate: true iff `actor` is a live, non-mirror, in-universe LOCAL Prop Element the host's
// snapshot did NOT claim (a divergence-sweep candidate). Shared with trash_collect_sync. Game thread.
bool IsInDivergenceUniverseUnclaimed(void* actor);

// IsInDivergenceUniverseUnclaimed gated on an actually-pending sweep (g_sweepPending). Game thread.
bool IsPendingSweepCandidate(void* actor);

// The load-tail quiescence signal (sticky g_sweepFired): true once the join load tail has drained + the sweep
// has run. Consumed by npc_adoption / kerfur_prop_adoption / kerfur_convert / quiescence_drain / kerfur_census
// / trash_collect_sync / event_dispatch_entity to gate their own quiescence-dependent work. Game thread.
bool HasLoadTailQuiesced();

// Session teardown (the net_pump disconnect / mid-snapshot-drop edge): clear the claim set, cancel any pending
// sweep, and Reset the downstream join modules (pile_spawn_bind + quiescence_drain + kerfur_reconcile +
// mirror_defer). The ONLY site that clears quiescence_drain's deferred queues. Game thread.
void ResetClaimTracking();

}  // namespace coop::join_membership_sweep
