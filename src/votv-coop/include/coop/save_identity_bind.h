// coop/save_identity_bind.h -- Phase 1 step 2b: CLIENT-side eid-range BIND of keyless save-loaded natives.
//
// THE GOAL (docs/COOP_STABLE_ID_SIDECAR.md S2.1/S3.6; mini-design phase1-eid-range-bind): give each keyless
// save-loaded native (chipPile | off-prop kerfur) a STABLE cross-peer identity = the HOST eid, by SPAWN ORDER.
// The host built + sent the {objectsData-index -> host-eid} map (1B + 2a, VERIFIED: 874 entries arrive intact).
// This step CONSUMES it: as the client's loadObjects spawns the natives (the BeginDeferred thunk, 1A-proved to
// catch every keyless load-spawn), the k-th keyless spawn is bound to the k-th map entry's host eid.
//
// THE BIND (mini-design S3) is the SAME terminal operation the position-adopt path already ships
// (remote_prop_spawn.cpp:575/594): retire the native's peer-range LOCAL element, then RegisterPropMirror the
// native at the HOST eid -- a host-range MIRROR (principle 3: the native ACTOR is the local rendering of the
// host-authoritative entity). The ONLY difference is the TRIGGER: host eid + spawn-order, not a 30 cm
// position-match. Collision is closed by construction (mini-design S4): the peer eid is freed before the host
// eid is touched (disjoint ranges); an already-bound E (the rare "host PropSpawn beat the bind" race) is
// rebindInPlace'd + the redundant host-spawned actor echo-destroyed.
//
// FIRST MUTATING STEP: everything before this (1A/1B/S8.2/2a) was read-only / receive+log. This mutates the
// element registry. Gated [dev] save_identity_bind=1 (CLIENT ini); absent/0 = a cheap no-op (the 2a transport
// still logs the map, nothing binds). When Phase 1 is proven the gate is removed (RULE 2) and the bind becomes
// the shipping identity path that retires the position-adopt crutch (Phase 4).
//
// Game-thread only (the BeginDeferred thunk + RegisterPropMirror are GT). The received map is set from the
// save_transfer client completion (net thread) under a small mutex; the bind reads it on the GT.
#pragma once

#include "coop/save_identity_map.h"  // IdMap, Family

namespace coop::save_identity_bind {

// True iff [dev] save_identity_bind=1 (latched once). When false every call below is a cheap no-op.
bool IsEnabled();

// CLIENT, when the save-transfer sidecar finished + parsed (save_transfer::MaybeFinishLocked_): hand the
// received {index->eid} map to the bind driver + ARM it (resets the spawn ordinal to 0). Called BEFORE the
// harness loads the slot (so the map is ready before loadObjects spawns the natives). Copies the map. No-op
// when disabled. Thread-safe (the completion can run on the net thread).
void SetReceivedMap(const coop::save_identity_map::IdMap& map);

// CLIENT BeginDeferred thunk (trash_collect_sync::OnBeginDeferredSpawnObserve client branch): the k-th
// keyless-family load-spawn. Binds `newActor` to map entry k's host eid (mini-design S3), validating the
// detected family against the entry's family (a mismatch = order desync -> LOUD log + disarm, never a wrong
// bind). No-op when disabled / not armed / map exhausted. Game thread.
void OnKeylessLoadSpawn(void* newActor, coop::save_identity_map::Family family);

// CLIENT load quiescence (remote_prop_spawn TickClientReconcile, the sweep-fire point -- same seam the 1A
// probe's EmitVerdictAtQuiescence uses): log the per-join bind summary (bound count, case i/ii breakdown,
// desync flag). No-op when disabled / not armed. Game thread.
void EmitBindSummary();

// CLIENT session end (save_transfer::OnDisconnect): drop the map + ordinal + the bound-native guard set.
void OnDisconnect();

}  // namespace coop::save_identity_bind
