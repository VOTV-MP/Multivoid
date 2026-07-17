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

#include "coop/props/save_identity_map.h"  // IdMap, Family
#include "coop/element/element.h"          // ElementId (UpdateChipSavePosAndGetOld)
#include "ue_wrap/core/types.h"                 // FVector (UpdateChipSavePosAndGetOld)

namespace coop::save_identity_bind {

// True iff [dev] save_identity_bind=1 (latched once). When false every call below is a cheap no-op.
bool IsEnabled();

// CLIENT, when the save-transfer sidecar finished + parsed (save_transfer::MaybeFinishLocked_): hand the
// received {array-index->eid} map to the bind driver + ARM it (splits the map into the two per-family lists +
// resets both per-family cursors to 0). Called BEFORE the harness loads the slot (so the map is ready before
// loadObjects spawns the natives). Copies the map. No-op when disabled. Thread-safe (net thread).
void SetReceivedMap(const coop::save_identity_map::IdMap& map);

// CLIENT BeginDeferred thunk (trash_collect_sync::OnBeginDeferredSpawnObserve client branch): a save-load
// spawn OF `family`. Binds `newActor` to its host eid by the family's PAIRING RULE (sidecar v3): a chipPile
// (keyless) binds by per-family ordinal cursor == its saveSlot array index; a kerfurOff (keyed) binds by its
// PORTABLE save key (FindKerfurEntryByKey_) -> a cross-peer-stable eid (the 15:55 retire-regression fix). A
// kerfur whose key isn't readable yet at this PRE-FinishSpawning seam is deferred to the quiescence sweep
// (BindUnboundReCreates) -- NEVER cursor-bound (that was the bug). No-op when disabled / not armed. Game thread.
void OnSaveLoadSpawn(void* newActor, coop::save_identity_map::Family family);

// CLIENT load quiescence (remote_prop_spawn TickClientReconcile, the sweep-fire point -- same seam the 1A
// probe's EmitVerdictAtQuiescence uses): log the per-join bind summary (bound count, chip cursor, case i/ii
// breakdown, any chip overflow; kerfurOff is key-bound -> no cursor). No-op when disabled / not armed. GT.
void EmitBindSummary();

// CLIENT quiescence (RunDivergenceSweep_, after SweepReconcileSaveTimeTwins, before ApplyPendingPosCorrections):
// re-bind save natives left UNBOUND after the seam, each by ITS intrinsic identity now that the natives are
// fully spawned. (1) a chipPile that UE's incremental GC destroyed + re-instantiated mid-join re-creates at its
// save position UNBOUND (the cursor was consumed -> the 09:54/11:32 ghost) -> POSITION match against the
// authoritative host-wire savePos (sidecar v2) within 1cm. (2) a kerfurOff whose key wasn't readable at the
// PRE-FinishSpawning seam -> KEY match against its portable save key (sidecar v3): the keyed family's GUARANTEED
// bind, never position, never cursor. Bound eids are skipped (no double-bind); co-located chips (>1 within 1cm)
// ambiguous-skip. No-op when disabled / not armed / a v1/v2 peer (no keys). GT. Returns the count re-bound.
// `ghostRetireDrained` (optional out): FALSE when the v106b GHOST-RETIRE tail left ghosts standing --
// the per-pass destroy cap (40) truncated the set, or the >50% mass-verdict valve aborted. The caller
// (quiescence_drain) must then KEEP the ghost sweep armed so the next pass finishes the drain
// (2026-07-10 audit: the uncapped tail could destroy ~49% of live natives in one tick).
int BindUnboundReCreates(bool* ghostRetireDrained = nullptr);

// b3 OWNER (docs/piles/12): the host's PropSnapPos says keyless save-pile `eid` is now at `newPos`. Record it
// as the entry's HOST-POS OVERLAY -- the immutable savePos is where the GAME re-creates the native on any
// purge/churn (loadObjects replays the save arrays), so it must keep naming that spot; the overlay is where
// the host says E belongs. BindUnboundReCreates then searches @host FIRST (a churned mirror's surviving actor
// -- the resurrect protection) and falls back to @save (a purge re-create), snapping a @save re-bind to @host
// via the pos-correction. (The pre-2026-07-03 code RETRACKED savePos itself -- a purge re-create could then
// never match and the eid went permanently unbindable: the eid=4435 client-local dup + the pinned 4 Hz drain.)
// Returns true + fills `saveOut` with the immutable save-pos when the pile genuinely MOVED (>50cm from @save)
// -- the caller then arms a host-vacate twin to retire the stale copy @save. Returns false for a small nudge
// (pos-correction alone handles it) or an eid not in the chip identity map. GT.
bool UpdateChipHostPos(coop::element::ElementId eid, const ue_wrap::FVector& newPos,
                       ue_wrap::FVector& saveOut);

// DEV PROBE (gated on [dev] ini `force_save_churn`, RULE-2-exempts-probes): deterministically reproduce the
// variant-1 precondition for a hands-on verify. The real engine-GC churn is non-deterministic (a sparse ~2 of
// 870), so a clean run may never exercise the chip position re-bind in BindUnboundReCreates (N=0). This UNBINDS
// the first N currently-bound chipPile save-natives (Takes their mirror Element; the actor stays alive at its
// save position) right before the quiescence sweep -- so the sweep then sees N unbound natives at save positions
// and re-binds them by the host-wire savePos. The verify: log shows `RE-BIND chipPile by position` N>0 binding
// the right native to the right eid. One-shot (latched). No-op unless the flag is set. Game thread (sweep tick).
void ForceSaveChurnForTest();

// DEV SELF-TEST (gated [dev] reseed_orphan_selftest=1, one-shot, runs wherever live chipPile natives exist --
// the host's s_1234 load). Deterministically reproduces the 09:54 re-seed-orphan race IN-PROCESS on a real
// save-native actor -- no save-transfer-join, no rendering, so it CANNOT false-green the way a clean smoke does.
// Self-arms a 1-entry map at a live native's position, binds it to a free host eid, Takes the Element + enqueues
// it deferred (the reaper's Take-but-not-Flushed window), then runs the fix sequence (ElementDeleter::Flush ->
// ReSeedKnownKeyedProps -> BindUnboundReCreates) and asserts the churned native is RE-BOUND to its host
// eid (not orphaned). Logs `VERDICT=PASS/FAIL`; restores the subject native after. Returns true on PASS. Requires
// save_identity_bind=1. Game thread. [[feedback-rule2-exempts-probes-diagnostics-tools]]
bool RunReseedOrphanSelfTest();

// CLIENT session end (save_transfer::OnDisconnect): drop the map + both per-family lists/cursors + the
// bound-native guard set.
void OnDisconnect();

}  // namespace coop::save_identity_bind
