#pragma once

// coop/remote_prop_spawn.h -- wire-driven PropSpawn receiver
// (extracted from coop/remote_prop.cpp M-1 2026-05-29 to bring that file
// under the 800-LOC soft cap; was 1028 LOC).
//
// Owns the OnSpawn pipeline that materializes a wire-received Prop on
// the receiver side:
//   1. Validate wire payload (class + key non-empty, key != "None").
//   2. EXACT-KEY DEDUP -- if a local Aprop_C derivative with the same
//      Key already exists, converge transform + (if collision-restore
//      class) restore collision + register mirror Element. Skip the
//      convergence write when the local actor is already under active
//      PropPose drive (the stream owns position while held).
//   3. FUZZY-POSITION DEDUP (Gap-I-1) -- if no exact-key match, look
//      for a same-class actor within 30 cm of the wire transform; if
//      found, converge transform + rekey via Aprop_C.setKey + restore
//      collision + register mirror. Drive-skip applies the same way.
//   4. FRESH SPAWN -- BeginDeferredActorSpawnFromClass + setKey BEFORE
//      FinishSpawningActor (so Aprop_C.Init() doesn't overwrite Key
//      with NewGuid) + FinishSpawningActor + optional initial physics
//      (SetSimulatePhysics + linear velocity).
//   5. Register the resulting actor as a Prop mirror at sender's eid.
//
// Cross-TU API to coop::remote_prop:
//   - remote_prop::IsActorUnderAnyDrive(actor) -- drive-cache predicate
//   - remote_prop::RegisterPropMirror(eid, actor, key, cls)
//   - remote_prop::KeyToWString(wireKey)
//
// NO echo loop: this path spawns via engine::SpawnActor (deferred-spawn
// pair) NOT through UpropInventory_C.takeObj, so the takeObj POST
// observer (the sender hook) never fires.

namespace coop::net {
struct PropSpawnPayload;
}  // namespace coop::net

namespace coop::remote_prop_spawn {

// Called from event_feed when a PropSpawn reliable message arrives.
// Game-thread only (UFunction calls are GT-only). event_feed posts
// via game_thread::Post to satisfy this. `senderSlot` is the reliable
// header's senderPeerSlot (host-relay logical origin) -- tagged onto the
// mirror for per-slot disconnect eviction (D1-7). `localPlayer` (the live
// AmainPlayer_C*, may be null) feeds the local-held guard: a prop the LOCAL
// player is grabbing is claimed + mirror-bound but never physics-reconciled
// or teleport-converged (Fork B 2d -- a re-bracket re-expresses held props).
// `fromConvert` (remote_prop::OnConvert's synthesized pile spawn only)
// disables the keyless-pile position-bind lane: a convert-born pile has no
// local counterpart by construction, so it must never bind to a nearby
// unrelated pile -- it always fresh-spawns. Wire dispatch passes the default.
void OnSpawn(const coop::net::PropSpawnPayload& payload, int senderSlot,
             void* localPlayer, bool fromConvert = false);

// ---- Adoption-universe claim tracking (P2 2026-06-10; widened Fork B 2e) --
//
// The client boots a FRESH New Game world and ADOPTS the host's prop world
// at every snapshot bracket. Contract: SWEEP == EXPRESS + CLIENT-FORBIDDEN.
// During the bracket, OnSpawn binds every host-expressed prop to a client
// actor (exact-key match, fuzzy match, eid match, or fresh spawn) -- each
// bind is a CLAIM; the SELF-announce broadcast sites claim their own actors
// too (RecordClaimIfTracking -- an entity expressed on the wire in EITHER
// direction is accounted for). At SnapshotComplete every still-live UNCLAIMED
// actor in the expressible universe -- keyed IsClassKeyedInteractable actors
// + the keyless chipPile lineage (eid-expressed by the host snapshot) -- is a
// local the host's world does not have: destroyed (echo-suppressed,
// OnDestroy-parity teardown) so the client adopts the host world. Keyless
// NON-pile actors (held clumps mid-flight, pre-Init Aprop_C, event clumps)
// are NOT expressible and never swept. The wire-suppressed mushroom7
// intermediate is keyed-in-universe but never expressed -> swept: parity
// with the client's connected-state destroy-on-sight.
//
// Protections by construction: baked-key placed props bind exact-key;
// unmoved runtime-keyed props bind fuzzy; wire fresh-spawns claim pre-
// Finish; locally-held props claim + skip reconcile (2d). A prop the host
// destroyed/no longer has is correctly swept -- that IS the adoption.
//
// Ordering is drain-order-safe: SnapshotBegin -> PropSpawn* -> SnapshotComplete
// arrive on one reliable lane and are dispatched INLINE on the game thread by
// the event_feed drain, so all claims strictly precede the sweep. The bracket
// is unicast to the joining peer (prop_snapshot SendReliableToSlot), so an
// already-connected peer never arms tracking nor sweeps. The host side only
// opens a bracket when its registry expresses its CURRENT world (the fork-A
// coherence gate) -- a mid-transition near-empty bracket can no longer reach
// the sweep.

// Arm claim tracking (clears any prior claim set). Called from the client's
// SnapshotBegin handler. Game-thread only.
void BeginClaimTracking();

// Public claim primitive for the SELF-announce broadcast sites (Init POST /
// takeObj POST / held-item / convert): a client announcing a prop while a
// bracket is open creates a live in-universe actor the host's already-
// enumerated bracket cannot express -- claim it or the sweep destroys the
// announcer's own prop while the host keeps the mirror. No-op when tracking
// is disarmed (one bool read). Game-thread only.
void RecordClaimIfTracking(void* actor);

// Sweep: destroy every live unclaimed actor of the expressible universe,
// then disarm tracking. `localPlayer` feeds the OnDestroy-parity teardown
// (PHC release if a doomed actor is somehow held). No-op (with a WARN) if
// tracking was never armed -- a SnapshotComplete without its Begin must not
// mass-destroy with an empty claim set. Called from the client's
// SnapshotComplete handler. Game-thread only.
void DestroyUnclaimedDivergentProps(void* localPlayer);

// Disarm + clear without sweeping. Called on session disconnect so a
// mid-snapshot drop does not leave dangling actor pointers armed across
// sessions. Game-thread only.
void ResetClaimTracking();

}  // namespace coop::remote_prop_spawn
