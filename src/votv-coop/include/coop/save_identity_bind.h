// coop/save_identity_bind.h -- Phase 1 step 2b: CLIENT-side eid-range BIND of keyless save-loaded natives.
//
// THE GOAL (docs/COOP_STABLE_ID_SIDECAR.md S2.1/S3.6; mini-design phase1-eid-range-bind): give each keyless
// save-loaded native (chipPile | off-prop kerfur) a STABLE cross-peer identity = the HOST eid, keyed on the
// saveSlot ARRAY INDEX (Build 3, per-family). The host built + sent the {array-index -> host-eid} map (1B + 2a +
// Path A, VERIFIED: 874 entries arrive intact, in [objectsData(kerfur) -> primitivesData(chip)] array order).
// This step CONSUMES it: as the client's loadObjects/Load-Primitives replay the saved arrays (caught at the
// BeginDeferred thunk, 1A-proved to catch every keyless load-spawn), the k-th spawn OF EACH FAMILY is bound to
// that family's k-th map entry == that family's array index k. RE-proved (loadObjects bytecode): each replay
// loop is a synchronous in-loop for-loop, so the within-array spawn order == array index order; only the
// CROSS-array phase order varies run-to-run, which per-family cursors are immune to (a global ordinal was not).
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
// received {array-index->eid} map to the bind driver + ARM it (splits the map into the two per-family lists +
// resets both per-family cursors to 0). Called BEFORE the harness loads the slot (so the map is ready before
// loadObjects spawns the natives). Copies the map. No-op when disabled. Thread-safe (net thread).
void SetReceivedMap(const coop::save_identity_map::IdMap& map);

// CLIENT BeginDeferred thunk (trash_collect_sync::OnBeginDeferredSpawnObserve client branch): the k-th
// load-spawn OF `family`. Binds `newActor` to that family's k-th map entry's host eid (Build 3 per-family
// cursor == that family's saveSlot array index). The family selects the list, so a cross-family mismatch is
// structurally impossible (no tripwire needed); a per-family count overflow stops only that family. No-op when
// disabled / not armed. Game thread.
void OnKeylessLoadSpawn(void* newActor, coop::save_identity_map::Family family);

// CLIENT load quiescence (remote_prop_spawn TickClientReconcile, the sweep-fire point -- same seam the 1A
// probe's EmitVerdictAtQuiescence uses): log the per-join bind summary (bound count, per-family cursors, case
// i/ii breakdown, any overflow). No-op when disabled / not armed. Game thread.
void EmitBindSummary();

// CLIENT session end (save_transfer::OnDisconnect): drop the map + both per-family lists/cursors + the
// bound-native guard set.
void OnDisconnect();

}  // namespace coop::save_identity_bind
