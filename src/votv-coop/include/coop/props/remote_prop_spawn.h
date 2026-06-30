#pragma once

#include <string>

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
struct WireClassName;
}  // namespace coop::net

namespace coop::remote_prop_spawn {

// Build a wide string from a wire class name (lossless for ASCII -- VOTV class
// names are ASCII). Shared so remote_prop::OnConvert can class-test a convert's
// pileClass (the HIGH-1 convert-before-spawn proxy form) without duplicating it.
std::wstring ClassNameToWString(const coop::net::WireClassName& cn);

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
// `deferKerfur` (K-6, default true for the wire dispatch): for a prop-form kerfur whose local twin
// isn't found by the inline fuzzy match, DEFER to the polled class+pose adoption
// (kerfur_prop_adoption::Arm) instead of fresh-spawning a duplicate beside the still-async-loading
// twin. Passed false by the adoption's own quiescence fresh-spawn fallback (one-shot, no defer loop)
// and by kerfur_convert::MaterializeKerfurMirror's convert materialize (the parked ghost is ready now).
//
// `outSpawned` / `skipBind` (v81 MORPH V2, remote_prop::OnConvert only): when `skipBind` is true,
// OnSpawn does NOT bind the freshly-spawned actor to `payload.elementId` (it skips the final
// RegisterPropMirror + IndexActorKey) and instead returns the actor via `*outSpawned`. The bind-model
// morph re-skins the SAME eid E in place, and the rebind path differs for a LOCAL element (host's own
// pile -> RebindLocalElementActor) vs a MIRROR (a bystander's adopted pile -> RegisterPropMirror
// rebindInPlace), so OnConvert binds explicitly. `fromConvert` ALSO skips the eid-dedup so a convert
// always fresh-spawns the new rendering of E (the still-live old rendering of E must NOT converge it).
void OnSpawn(const coop::net::PropSpawnPayload& payload, int senderSlot,
             void* localPlayer, bool fromConvert = false, bool deferKerfur = true,
             void** outSpawned = nullptr, bool skipBind = false);


// The join-window claim-tracking + divergence sweep (BeginClaimTracking / RecordClaimIfTracking /
// ArmDivergenceSweep / TickClientReconcile / HasLoadTailQuiesced / IsInDivergenceUniverseUnclaimed /
// IsPendingSweepCandidate / OnClientWorldReadyResetSweep / ResetClaimTracking) was EXTRACTED to
// coop/props/join_membership_sweep.h 2026-06-30 (anti-smear: this file was over the 1500 LOC hard cap;
// the membership-claim+sweep is a distinct concept from the OnSpawn receiver). OnSpawn records into it.

}  // namespace coop::remote_prop_spawn
