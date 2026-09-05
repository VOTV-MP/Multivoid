// coop/element/quiescence_drain.h -- the join-window order owner: the drain-edge reconcile
// sequence that, at load-tail quiescence, reconciles divergent local-versus-authoritative
// element identity in a fixed order; it holds every deferred queue armed during the join
// window and drains them in sequence, and nothing else sequences this axis. The sequence:
// retire a stale native chip pile at its old position; re-bind purge-churned unbound
// natives; retire a stale kerfur off-prop (both distinct modules this calls); apply a
// destroy that raced ahead of the bind, after the rebind; snap a window-moved save pile to
// the host position. Two triggers, both at or after quiescence: the join-window fire edge
// (the membership sweep's reconcile tick), which runs this before the membership doom while
// claim tracking is armed, so a reconcile that converge-binds a re-create claims it and the
// sweep spares it; and a steady-state throttled tick, firing whenever armed work is left
// past quiescence. Event handlers only arm the queues here and never apply; this module
// alone applies, in order, and a mutation that cannot resolve now stays queued, never
// dropped. Not owned here: the membership doom sweep, a join-window one-shot, since in
// steady state it would wipe a legitimately diverged world. Game thread only, no mutex.

#pragma once

#include "coop/element/element.h"  // coop::element::ElementId
#include "coop/props/join_membership_sweep.h"
#include "coop/net/protocol.h"     // PropDestroyPayload
#include "ue_wrap/core/types.h"         // ue_wrap::FVector / FRotator

namespace coop::element::quiescence_drain {

// The ordered reconcile sequence (the header's steps). Called at the join-window quiescence
// fire edge, before the doom sweep, and by the steady-state tick. Game thread only.
void RunReconcile();

// The steady-state trigger; call every client reconcile tick. Past load-tail quiescence,
// when there is pending work and the debounce interval has elapsed, runs the sequence.
// Cheap when idle: a pending-work poll and a time compare, no object-array walk unless
// there is work. Game thread only.
void OnTick();

// The queue arm entry points: event handlers capture here and never apply.

// A land carried a save-time key (the host self-seeded the eid at an in-window grab and
// stamped the pre-grab position), or the pile spawn bind's twin missed at world-ready. Arm a
// pending save-time twin so the sweep retires the stale native at the old position at
// quiescence. Idempotent per eid; the latest wins.
void ArmPendingSaveTimeTwin(coop::element::ElementId eid, const ue_wrap::FVector& savePos, uint8_t chipType);

// Armed from a host position correction: the host authoritatively moved E off `oldPos`, so
// that save position is vacated, and the sweep retires whatever save-loaded native lingers
// there on the host's word (no client-side position guess and no majority cap; the guess is
// what pointer reuse corrupted). The host-mutated-in-window authority the client heuristics
// only inferred.
void ArmHostVacateTwin(coop::element::ElementId eid, const ue_wrap::FVector& oldPos);

// A join-window position correction for a save-authoritative chip pile the host moved while
// the joiner's reliable channel was not ready. Armed on receipt; the latest wins. Applied at
// quiescence, or immediately by the caller via ApplyPendingPosCorrections if already
// quiesced.
void ArmPendingPosCorrection(coop::element::ElementId eid,
                             const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot);

// Drain the armed corrections (applied ones erased). Called from the sequence and, for a
// late arrival after the sweep already fired and bound, immediately from the receive
// handler; the immediate apply is not an order violation, since it runs only
// post-quiescence, when the order no longer gates. Bounded: a correction whose eid never
// binds is dropped loudly after the pass cap, since one unbindable eid otherwise pinned the
// pending-work flag and the full-array drain forever.
void ApplyPendingPosCorrections();

// The arm-if-absent variant, the save-position re-bind assist: the identity bind re-bound a
// purge re-create at its save position for an eid the host says is elsewhere, so ensure a
// correction exists and the drain snaps it to the host position. An already-armed
// correction (a fresher host-sent rotation) is kept.
void EnsurePosCorrection(coop::element::ElementId eid,
                         const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot);

// The save-position re-bind claimed the native at the twin's key as E's own re-create: there
// is no stale copy, so the pending twin's premise is dead. Cancel it (idempotent) instead of
// letting it burn its pass cap of walk and log noise against a now-bound, never-matchable
// candidate.
void CancelPendingSaveTimeTwin(coop::element::ElementId eid);

// Destroy before load: a destroy can arrive before this peer has loaded its copy of the
// doomed save-loaded prop. The destroy receiver finds no local actor and arms it here
// instead of dropping it (the prop would later load unopposed, a duplicate); the sequence
// re-applies it after the bind through the destroy re-apply, so destroy delivery becomes
// order-independent. Since the join barrier this is unreachable in the join window (no
// destroy arrives pre-quiescence); it remains live for the travel window (host sends flow
// while a cave or level reload churns, with no travel-start gate) and the probe-deadline
// degraded mode.
void ArmPendingDestroy(const coop::net::PropDestroyPayload& payload);

// True iff there is armed but unconsumed reconcile work (a pending save-time twin, position
// correction, destroy or kerfur retire). The tick polls this so it only walks when there is
// something to reconcile. Game thread only.
bool HasPendingWork();

// The ghost-sweep arm: an event stranded (or may have stranded) an identity-less native chip
// pile on this client (a rebind displaced a live native, or a use press landed on an unbound
// native post-quiescence). Arming makes the next reconcile pass run, whose re-bind step's
// ghost-retire tail adjudicates every such ghost at once: re-bind what a map key claims,
// retire the provably identity-less rest. Event handlers capture here; the sequence applies.
// Game thread only.
void ArmGhostSweep();

// Drop the deferred queues (save-time twins, position corrections, destroys). Called only at
// session teardown (the membership sweep's claim-tracking reset, the disconnect and
// world-drop edge). The queues deliberately survive bracket close: they drain at quiescence
// or in steady state, never per bracket (a per-bracket reset once lost an undrained
// correction). Game thread only.
void Reset();

}  // namespace coop::element::quiescence_drain
