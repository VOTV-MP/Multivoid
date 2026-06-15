// coop/trash_collect_sync.h -- mirror a freshly-spawned-and-grabbed item
// (the VOTV trash-pile collect).
//
// Pressing E on a trashBitsPile_C ("trash stack") spawns one Aprop_C trash
// item and auto-grabs it into the player's hands the same frame. The item is
// born with Key == None (its BP UCS hasn't minted a NewGuid yet), so our
// held-prop pose stream emitted unmatchable PropPose key='None' AND the
// Init-POST broadcast skipped it -> the held item never mirrored to peers.
//
// We CANNOT catch the collect via a UFunction observer: trashBitsPile_C::
// playerTryToCollect is dispatched BP->BP (ProcessInternal), which bypasses
// our ProcessEvent detour entirely (verified hands-on: a POST observer
// installed but NEVER fired while the item was clearly held). So detection
// happens where the engine state IS visible -- net_pump's held-prop send,
// which already resolves the grabbing_actor every tick.
//
// EnsureHeldItemBroadcast is called on the NEW-held edge from that send: if
// the held actor is a freshly-spawned UNKEYED Aprop_C, it force-mints a stable
// Key and broadcasts a PropSpawn so every peer spawns a mirror. From there the
// existing grabbing_actor pose stream carries the item into the collector's
// hands, and PropRelease/PropDestroy unwind it like any held prop.
//
// CRASH-SAFE for the non-Aprop_C garbageClump/chipPile (the actual trash): we
// DO broadcast them, but the receiver never runs physics on them. GetStaticMesh
// returns null for non-Aprop_C, so the clump mirror spawns physics-free and is
// driven KINEMATICALLY (per-tick SetActorLocation, no mesh) -- the inverse of
// the reverted-2a use-after-free, which resolved their real mesh and physics-
// drove a self-morphing actor. [[project-bug-trash-chippile-uaf-crash]]

#pragma once

#include <cstdint>

#include "ue_wrap/types.h"  // FVector (the WatchPileAt position overload)

namespace coop::net { class Session; }

namespace coop::trash_collect_sync {

// Game thread. If `heldActor` is a live, UNKEYED (Key=None) Aprop_C, force-mint
// a stable Key on it and broadcast a PropSpawn under that Key (so peers spawn a
// mirror the held-pose stream can then drive into the collector's hands).
// Returns true iff it minted + broadcast. No-op (returns false) for: null/dead
// actors, non-Aprop_C (transient chip/clump -- crash safety), and already-keyed
// actors (a normal world-prop grab -- the peer already has it). Idempotent:
// once minted the Key is non-None, so a repeat call returns false.
//
// For the non-keyable trash CLUMP (key=None, eid-only) it ALSO registers the clump
// in the death-watch set (below) so its UNOBSERVABLE morph-destroy gets a despawn.
bool EnsureHeldItemBroadcast(void* heldActor, coop::net::Session* session);

// Game thread, per-tick. The trash clump's destruction (re-pile on landing /
// LifeSpan expiry) is dispatched natively/BP-internally and NEVER reaches our
// K2_DestroyActor ProcessEvent observer (verified hands-on 2026-06-03: every grab
// broadcast a fresh clump eid but ZERO destroys -> the peer's clump mirrors piled up
// = the infinite grab/throw dupe). So the OWNER watches each clump it broadcast and,
// the tick that clump's actor goes dead, broadcasts a PropDestroy(key=None, eid) for
// the peer to despawn the mirror -- the MTA owner-authoritative destroy, driven by
// liveness instead of an (unobservable) destroy edge. O(1)/tick over a tiny bounded
// set. [[project-bug-trash-chippile-uaf-crash]]
void TickWatchReleasedClumps(coop::net::Session* session);

// ---- mirror-pile death-watch (v52: identity-based clump re-grab destroy) -----------------
//
// A trash pile (the ball's convert product) exists as TWO cross-peer entities sharing one eid:
// the owner's authoritative pile + the receiver's mirror pile. When ANY peer grabs the shared
// pile, the pile morphs into a held clump and self-destructs LOCALLY on the grabber's machine
// (actorChipPile_C::playerGrabbed -> turnToPile -> K2_DestroyActor, all BP-internal/unobservable).
// So whoever grabs it, the grabber's machine watches the pile die and broadcasts PropDestroy(eid)
// -> the other peers drop their mirror. This is the robust, identity-exact replacement for the
// retired InpActEvt_use lookAtActor grab-guess (which was a single-edge heuristic that fired only
// on the grabber and only via a fiddly aim+press disambiguation).
//
// Register a pile (owner's just-converted pile OR a receiver's freshly-spawned mirror pile) under
// its cross-peer eid. Idempotent per eid. Game thread. `quiet` suppresses the per-pile enroll log
// (used by the host snapshot's bulk eager-enroll of ~870 piles, which emits ONE summary line itself).
void WatchPile(void* pileActor, uint32_t eid, bool quiet = false);

// Same, with a caller-provided position (perf audit W-3 2026-06-10: the
// snapshot drain already read the pile's location for the payload -- this
// overload skips the duplicate GetActorLocation dispatch per expressed pile).
void WatchPileAt(void* pileActor, uint32_t eid, const ue_wrap::FVector& pos, bool quiet = false);

// Per-tick liveness sweep over watched piles. When a watched pile's actor goes dead AND it was
// NEAR the local camera (a grab happens AT the player) AND we're not in a world-transition window
// (`suppress` -- the connect-teleport sublevel stream-out makes far piles go dead; the grime
// super-sponge precedent), broadcast PropDestroy(key=None, eid). Far / transition deaths are
// stream-outs -> ignored. O(1)/tick over a tiny bounded set. Game thread.
void TickWatchReleasedPiles(coop::net::Session* session, bool suppress);

// A watched pile was torn down by an INCOMING PropDestroy/PropConvert (the OTHER peer grabbed it):
// drop its watch entry so the next liveness sweep doesn't re-broadcast its (wire-induced) death
// back to the origin. Game thread. No-op if the eid isn't watched.
void NotifyPileConsumed(uint32_t eid);

// Drop all watched clumps + piles (full session teardown / aggregate disconnect).
void OnDisconnect();

}  // namespace coop::trash_collect_sync
