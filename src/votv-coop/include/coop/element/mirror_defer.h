// coop/mirror_defer.h -- the instant-world UPPER layer: deferred-spawn visibility.
//
// A joining client briefly sees a "dance" -- duplicate props and kerfurs flickering in, ghosts,
// wrong positions, self-correcting over a second or two -- because host mirrors spawn VISIBLE
// before the quiescence-gated reconcile resolves them. This module hides each freshly spawned
// host mirror at the spawn choke-points and then reveals it, either at the curtain-lift for the
// confirmed ones or at the quiescence backstop for the rest. The reconcile underneath -- the
// quiescence drain and the kerfur reconcile sweeps -- is UNTOUCHED: this is a pure visibility
// layer over it, so the worst case is the dance as it is today and the best case is instant.
//
// The CONFIRMED-versus-HOLD discriminator is the spawn payload's `hasMatchPos` flag, passed in
// by the hook site rather than queried out of the reconcile pending sets, which is what keeps
// the layer separable. A mirror carrying a save-time key is exactly the one whose local twin
// stays visible until the quiescence sweep, so it HOLDs; one without is a host-only or derived
// form with no local twin, so it is CONFIRMED and revealed at the lift. Game thread ONLY.

#pragma once

#include <cstdint>

namespace coop::mirror_defer {

// Arm the deferred-hide window (CLIENT, at connect / StartCoopSession). Until disarmed,
// OnMirrorSpawned hides each new host mirror. Idempotent; clears any stale tracking.
void Arm();

// Disarm + drop all tracking (session end / teardown). Does NOT reveal -- teardown destroys.
void Reset();

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
