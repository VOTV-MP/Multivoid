// coop/mirror_defer.h -- instant-world UPPER layer (SEAM 2+3): deferred-spawn visibility.
//
// The joining client briefly sees a "dance" (dup props/kerfurs flicker in, ghosts, wrong
// positions, self-correct over ~1-2s) because host mirrors spawn VISIBLE before the
// quiescence-gated reconcile resolves them. This module hides each freshly-spawned host
// mirror at the spawn choke-points, then reveals it -- either at the curtain-lift (the
// confirmed ones) or at the quiescence backstop (the rest). The reconcile BACKUP
// (quiescence_drain / kerfur_reconcile sweeps) is UNTOUCHED -- this is a pure visibility
// layer on top of it (worst case = today's dance, best case = instant). See
// docs/COOP_INSTANT_WORLD_TWO_LAYER.md.
//
// The CONFIRMED/HOLD discriminator is the spawn payload's `hasMatchPos` flag, passed in by
// the hook site (NOT a query into the reconcile pending sets -- the backup stays untouched,
// git diff reconcile == 0): a mirror carrying a save-time key (hasMatchPos) is exactly the
// one whose local twin is still visible until the quiescence sweep -> HOLD; one without is a
// host-only/derived form with no local twin -> CONFIRMED (reveal at lift).
//
// Game-thread ONLY (every entry runs in the event_feed drain / the client reconcile tick).

#pragma once

#include <cstdint>

namespace coop::mirror_defer {

// Arm the deferred-hide window (CLIENT, at connect / StartCoopSession). Until disarmed,
// OnMirrorSpawned hides each new host mirror. Idempotent; clears any stale tracking.
void Arm();

// Disarm + drop all tracking (session end / teardown). Does NOT reveal -- teardown destroys.
void Reset();

// True while the join deferred-hide window is open. The spawn hooks gate on this so
// steady-state gameplay spawns (post-quiescence) are NEVER hidden.
bool IsArmed();

// A fresh host-mirror actor was just spawned AND registered into its MirrorManager. Hide it
// (SetActorHiddenInGame); when `collisionOff`, also SetActorEnableCollision(false) so it is
// not grab-trace-hittable / physics-active while invisible (pile proxies are already
// collision-less -> pass false for them). `holdUntilQuiescence` = the payload's hasMatchPos
// (a save-time-keyed mirror whose local twin is still visible -> hold past the lift reveal).
// No-op unless armed. `eid` is the host element id (tracking key).
void OnMirrorSpawned(uint32_t eid, void* actor, bool collisionOff, bool holdUntilQuiescence);

// Curtain-lift reveal ("primary world assembled" = SnapshotComplete + spawn-drain, NOT
// g_sweepFired): reveal every hidden mirror NOT held-to-quiescence. The held ones (their
// local twin still visible) stay hidden -> revealed by the quiescence backstop below.
void RevealConfirmedAtLift();

// Quiescence backstop (call AFTER RunDivergenceSweep_ at the g_sweepFired flip): reveal every
// still-hidden survivor + disarm. The catch-all -- every hidden mirror was tracked here, so
// nothing can stay stuck hidden (a swept ghost is dead -> liveness-gated, skipped).
void RevealAllSurvivorsAtQuiescence();

}  // namespace coop::mirror_defer
