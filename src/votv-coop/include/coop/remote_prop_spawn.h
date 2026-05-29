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
// via game_thread::Post to satisfy this.
void OnSpawn(const coop::net::PropSpawnPayload& payload);

}  // namespace coop::remote_prop_spawn
