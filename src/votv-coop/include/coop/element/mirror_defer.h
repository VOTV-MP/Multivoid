// coop/element/mirror_defer.h -- the instant-world UPPER layer: deferred-spawn visibility.
//
// A joining client briefly sees a "dance" -- duplicate props and kerfurs flickering in, ghosts,
// wrong positions, self-correcting over a second or two -- because host mirrors spawn VISIBLE
// before the quiescence-gated reconcile resolves them. This module hides each freshly spawned
// host mirror at the spawn choke-points and then reveals it, either at the curtain-lift for the
// confirmed ones or at the quiescence backstop for the rest. The reconcile underneath -- the
// quiescence drain and the kerfur reconcile sweeps -- is UNTOUCHED: this is a pure visibility
// layer over it, so the worst case is the dance as it is today and the best case is instant.
//
// CONFIRMED versus HOLD is decided by the caller, not by a query into the reconcile pending
// sets, which is what keeps the layer separable. A prop spawn passes the payload's `hasMatchPos`
// flag: a mirror carrying a save-time key is exactly the one whose local twin stays visible
// until the quiescence sweep, so it HOLDs, while one without has no local twin and is revealed
// at the lift. An NPC mirror has no such twin: it never holds, and hides with its collision Off. Game thread ONLY.

#pragma once

#include <cstdint>

namespace coop::mirror_defer {

// Arm the deferred-hide window (CLIENT, at connect / StartCoopSession). Until disarmed,
// OnMirrorSpawned hides each new host mirror. Idempotent; clears any stale tracking.
void Arm();

// Disarm + drop all tracking (session end / teardown). Does NOT reveal -- teardown destroys.
void Reset();

// What a hide does to the mirror's collision; the reveal restores exactly that.
//   Keep         nothing: a pile proxy is already collision-less.
//   Off          SetActorEnableCollision(false): not grab-trace-hittable and not physics-active while
//                invisible. For a mirror that must not touch the world (a parked NPC, a held mirror).
//   PhysicsOnly  every primitive the actor owns (the root and its attached components, at any depth; an
//                attached actor's are its own) keeps only its physics facet: no query trace reaches the
//                mirror, but a simulating one still lands and rests on the floor as the host's copy did.
//                A primitive with physics becomes PhysicsOnly, a query-only one None, one with none stays;
//                the values found are put back at the reveal. An actor with more than 16 primitives, a
//                primitive that does not read or write, or a root with no physics facet is hidden as Off
//                instead, with nothing of PhysicsOnly applied.
enum class HideCollision : uint8_t { Keep, Off, PhysicsOnly };

// What a hide did, for a drill's evidence: the mode applied, the primitives a PhysicsOnly hide walked, and why it
// fell back to Off (null when it did not).
struct HideInfo {
    HideCollision applied;
    int primitives;
    const char* fallbackWhy;
};
// True with `out` when `eid` is hidden now.
bool HideInfoOf(uint32_t eid, HideInfo& out);

// A fresh host-mirror actor was just spawned AND registered into its MirrorManager. Hide it
// (SetActorHiddenInGame) and treat its collision as `collision` says. `holdUntilQuiescence` = the
// payload's hasMatchPos (a save-time-keyed mirror whose local twin is still visible -> hold past the
// lift reveal). No-op unless armed. `eid` is the host element id (tracking key).
void OnMirrorSpawned(uint32_t eid, void* actor, HideCollision collision, bool holdUntilQuiescence);

// Curtain-lift reveal ("primary world assembled" = SnapshotComplete + spawn-drain, NOT
// g_sweepFired): reveal every hidden mirror NOT held-to-quiescence. The held ones (their
// local twin still visible) stay hidden -> revealed by the quiescence backstop below.
void RevealConfirmedAtLift();

// Quiescence backstop (call AFTER RunDivergenceSweep_ at the g_sweepFired flip): reveal every
// still-hidden survivor + disarm. The catch-all -- every hidden mirror was tracked here, so
// nothing can stay stuck hidden (a swept ghost is dead -> liveness-gated, skipped).
void RevealAllSurvivorsAtQuiescence();

}  // namespace coop::mirror_defer
