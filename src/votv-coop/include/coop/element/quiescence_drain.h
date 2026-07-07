// coop/element/quiescence_drain.h -- the join-window ORDER owner.
//
// ONE concept, ONE owner: the drain-edge reconcile SEQUENCE that, at load-tail quiescence,
// reconciles divergent local-vs-authoritative element identity in a FIXED order. It holds
// every deferred queue armed during the join window and drains them in sequence; nothing
// else sequences this axis. [[feedback-one-owner-order-axis]]
//
// THE SEQUENCE (RunReconcile):
//   1. SweepReconcileSaveTimeTwins()                 -- retire a stale native chipPile@old (pile twin)
//   2. save_identity_bind::BindUnboundReCreates()    -- re-bind GC-churned unbound natives (identity layer; a
//                                                       distinct module the sequence CALLS, not absorbed)
//   3. kerfur_reconcile::SweepReconcileSaveTimeKerfurs() -- retire a stale kerfur off-prop (kerfur retire
//                                                       MECHANISM; another distinct module the sequence CALLS)
//   4. ApplyPendingDestroys()                        -- apply a destroy that raced ahead of the bind
//                                                       (destroy-before-load), AFTER the rebind so it resolves
//   5. ApplyPendingPosCorrections()                  -- snap a window-moved save-pile to the host pos (b3)
//   6. (joinSweep only) LogCensus()                  -- the one-shot L1 orphan census
//
// TWO TRIGGERS, both at/after quiescence:
//   - the join-window quiescence sweep (join_membership_sweep::RunDivergenceSweep_, joinSweep=true), and
//   - a steady-state throttled tick (OnTick) -- fires whenever there is armed-but-unconsumed work past
//     load-tail quiescence (the D1 structural fix: a save-pile grabbed/moved AFTER the join one-shot, or a
//     kerfur turned on when no pile bracket armed, would otherwise leave a pending queue nothing drains).
//
// CAPTURE vs SEQUENCE: event handlers (remote_prop::OnDestroy, the PropSnapPos handler, pile_spawn_bind's
// twin miss, npc_mirror's kerfur EntitySpawn) only ARM the queues here -- they NEVER apply. This module is
// the SOLE place that applies, in order. A mutation that can't resolve now (target not loaded) stays queued
// and re-applies next drain -- NEVER dropped (that is the destroy-before-load / D1 bug class).
//
// What it does NOT own: the MEMBERSHIP doom sweep (destroy locals the host's snapshot didn't claim + the
// >50% valve) stays a join-window one-shot in remote_prop_spawn -- running it in steady state would wipe a
// legitimately-diverged world. Only the per-eid identity reconcile (bounded to armed work) is safe to run
// steadily, and that is what lives here.
//
// CONSOLIDATED 2026-06-30 (anti-smear refactor) from: coop/element/identity_reconcile.cpp (the SEQUENCE +
// trigger) + coop/props/pile_reconcile.cpp groups B+C (the deferred QUEUES it drained) -- two halves of one
// concept across two folders + the generic kerfur destroy queue in a "pile"-named file. The kerfur retire's
// SEQUENCING also moved here (it was a 3rd parallel order owner in kerfur_convert::PollKerfurConversions).
//
// Game-thread ONLY (both triggers run on the game thread). No mutex of its own.

#pragma once

#include "coop/element/element.h"  // coop::element::ElementId
#include "coop/props/join_membership_sweep.h"
#include "coop/net/protocol.h"     // PropDestroyPayload
#include "ue_wrap/types.h"         // ue_wrap::FVector / FRotator

namespace coop::element::quiescence_drain {

// The ordered reconcile sequence (see the file header for the 6 steps). `joinSweep`=true at the join-window
// quiescence sweep (also logs the one-shot orphan census); false on a steady-state pass. Game-thread only.
void RunReconcile(bool joinSweep);

// Steady-state trigger. Call every client reconcile tick. Past load-tail quiescence, when there is pending
// reconcile work (HasPendingWork) and the debounce interval has elapsed, runs RunReconcile(false). Cheap when
// idle: a pending-work bool poll + a time compare, NO GUObjectArray walk unless there is actually work (the
// perf rule). Game-thread only.
void OnTick();

// ---- Queue ARM entry points (event handlers CAPTURE here; they never apply) ----

// docs/piles/09: a kToPile LAND carried a save-time key (the host self-seeded the eid at an in-window grab +
// stamped the pre-grab pos), OR pile_spawn_bind's twin missed at world-ready. Arm a pending save-time twin so
// SweepReconcileSaveTimeTwins retires the stale native@old at quiescence. Idempotent per eid (latest wins).
void ArmPendingSaveTimeTwin(coop::element::ElementId eid, const ue_wrap::FVector& savePos, uint8_t chipType);

// b3 OWNER (docs/piles/12): armed from a host PropSnapPos -- the host AUTHORITATIVELY moved E off `oldPos`, so
// that save-pos is vacated. The sweep retires whatever save-loaded native@old lingers there on the host's word
// (no client-side position-confirm guess, no >50% cap -- the guess is what GC pointer-reuse corrupted). This is
// the missing "host mutated eid in-window -> retire the old" authority the client heuristics only inferred.
void ArmHostVacateTwin(coop::element::ElementId eid, const ue_wrap::FVector& oldPos);

// b3 (v90, PropSnapPos): a join-window position correction for a save-authoritative chipPile the host MOVED
// while the joiner's reliable channel wasn't ready. Arm it on receipt; the latest wins. Applied at quiescence
// (or immediately by the caller via ApplyPendingPosCorrections if already quiesced).
void ArmPendingPosCorrection(coop::element::ElementId eid,
                             const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot);

// Drain the armed b3 corrections (applied ones erased). Called from the sequence AND, for a late arrival
// after the sweep already fired + bound, immediately from the receive handler (event_dispatch_entity).
// The immediate-apply is NOT an order violation: it only runs post-quiescence, when the order no longer gates.
// BOUNDED (2026-07-03): a correction whose eid never binds is dropped LOUD after kMaxPosCorrectionPasses --
// twins and deferred destroys were pass-capped, pos-corrections were not, and one unbindable eid pinned
// HasPendingWork -> the 4 Hz full-array drain forever (docs/piles/12 eid=4435).
void ApplyPendingPosCorrections();

// Arm-if-absent variant (2026-07-03, the savePos re-bind assist): save_identity_bind re-bound a purge
// re-create at its save position for an eid the host says is elsewhere -- ensure a correction exists so the
// drain snaps it to the host pos. An already-armed correction (fresher host-sent rotation) is kept as-is.
void EnsurePosCorrection(coop::element::ElementId eid,
                         const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot);

// The savePos re-bind claimed the native at the twin's key AS E's own re-create -- there is no stale copy, so
// the pending twin's premise is dead. Cancel it (idempotent) instead of letting it burn kMaxTwinPasses of
// 4 Hz walk+log noise against a now-bound (never-matchable) candidate.
void CancelPendingSaveTimeTwin(coop::element::ElementId eid);

// DESTROY-BEFORE-LOAD (2026-06-30): a PropDestroy can arrive BEFORE this peer has loaded its copy of the
// doomed save-loaded prop. remote_prop::OnDestroy finds "no local actor" and ARMS it here instead of dropping
// it (-> the prop later loads unopposed = a dup, the 5-vs-7 race). The sequence re-applies it AFTER the bind
// via remote_prop::TryApplyDestroy, so destroy delivery becomes order-independent.
void ArmPendingDestroy(const coop::net::PropDestroyPayload& payload);

// True iff there is armed-but-unconsumed reconcile work (a pending save-time twin OR a pending b3 position
// correction OR a pending destroy OR a pending kerfur retire). OnTick polls this so it only walks when there
// is something to reconcile. Game-thread only.
bool HasPendingWork();

// v106b GHOST-SWEEP arm (2026-07-07): an event stranded (or may have stranded) an identity-less native
// chipPile on this client -- a rebind displaced a live native (identity_create HOST RE-ASSERT), or an
// E-press landed on an unbound native post-quiescence. Arming makes the next reconcile pass run, whose
// step 2 (BindUnboundReCreates GHOST-RETIRE tail) adjudicates EVERY such ghost at once: re-bind what a
// map key claims, retire the provably identity-less rest. Event handlers CAPTURE here; the sequence
// applies -- the same contract as every other queue in this module. Game-thread only.
void ArmGhostSweep();

// Drop the deferred queues (save-time twins + pos-corrections + destroys). Called ONLY at session teardown
// (join_membership_sweep::ResetClaimTracking, the disconnect/world-drop edge). The queues deliberately SURVIVE
// bracket close -- they drain at
// quiescence / steady-state, never dropped per-bracket (that was the latent "Reset DROPPING undrained
// pos-correction" data loss the anti-smear split removed). Game-thread only.
void Reset();

}  // namespace coop::element::quiescence_drain
