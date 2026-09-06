// coop/props/save_identity_bind.h -- CLIENT-side eid-range BIND of keyless save-loaded
// natives.
//
// Gives each keyless save-loaded native (a chipPile or an off-prop kerfur) a stable
// cross-peer identity, the HOST eid, keyed per family on the ORDER the save arrays are
// replayed in. The host sends a map whose entries are in that order; as the client's
// loadObjects and Load-Primitives replay the saved arrays, caught at the BeginDeferred
// thunk, the k-th spawn OF EACH FAMILY binds to that family's k-th entry. Each replay
// loop is a synchronous for-loop, so within an array the spawn order IS the index order;
// only the cross-array phase order varies, which per-family cursors are immune to and one
// global ordinal would not be.
#pragma once

#include "coop/props/save_identity_map.h"  // IdMap, Family
#include "coop/element/element.h"          // ElementId (UpdateChipSavePosAndGetOld)
#include "ue_wrap/core/types.h"                 // FVector (UpdateChipSavePosAndGetOld)

namespace coop::save_identity_bind {

// True iff [dev] save_identity_bind=1 (latched once). When false every call below is a cheap no-op.
bool IsEnabled();

// CLIENT, when the save-transfer sidecar has arrived and parsed: hand the received
// {array index -> eid} map to the bind driver and ARM it, which splits the map into the two
// per-family lists and resets both cursors. Called BEFORE the harness loads the slot, so the
// map is ready before loadObjects spawns the natives. Copies the map. No-op when disabled.
// Thread-safe (net thread).
void SetReceivedMap(const coop::save_identity_map::IdMap& map);

// CLIENT BeginDeferred thunk (trash_collect_sync's client branch): a save-load spawn of
// `family`. Binds `newActor` to its host eid by that family's PAIRING RULE -- a keyless
// chipPile binds by the per-family ordinal cursor, which is its saveSlot array index; a keyed
// kerfurOff binds by its PORTABLE save key, giving a cross-peer-stable eid. A kerfur whose key
// is not readable yet at this PRE-FinishSpawning seam is deferred to the quiescence sweep and
// never cursor-bound. No-op when disabled or not armed. Game thread.
void OnSaveLoadSpawn(void* newActor, coop::save_identity_map::Family family);

// CLIENT load quiescence, at the sweep-fire point in remote_prop_spawn's client reconcile: log
// the per-join bind summary -- bound count, chip cursor, the case breakdown and any chip
// overflow. A kerfurOff is key-bound and so has no cursor. No-op when disabled or not armed.
// Game thread.
void EmitBindSummary();

// CLIENT quiescence, after the save-time twin reconcile and before the pending pos-corrections:
// re-bind save natives left UNBOUND after the seam, each by its intrinsic identity now that the
// natives are fully spawned. A chipPile that UE's incremental GC destroyed and re-instantiated
//   mid-join re-creates at its save position with its cursor already consumed, so it matches by
//   POSITION against the authoritative host-wire savePos, within 1 cm. A kerfurOff whose key was
//   unreadable at the PRE-FinishSpawning seam matches by KEY against its portable save key: the
//   keyed family's guaranteed bind, never position, never cursor.
// Bound eids are skipped and co-located chips (more than one within 1 cm) are ambiguous and
// skipped. No-op when disabled, not armed, or against a peer too old to send keys. Game thread;
// returns the count re-bound.
//
// `ghostRetireDrained` (optional out) is FALSE when the ghost-retire tail left ghosts standing:
// the per-pass destroy cap of 40 truncated the set, or the majority-verdict valve aborted -- the
// cap being what stops a runaway tail from destroying half the live natives at once. The caller
// must then KEEP the ghost sweep armed so the next pass finishes the drain.
int BindUnboundReCreates(bool* ghostRetireDrained = nullptr);

// The host's PropSnapPos says keyless save-pile `eid` is now at `newPos`. Record it as the
// entry's HOST-POS OVERLAY: the immutable savePos is where the GAME re-creates the native on
// any purge or churn, since loadObjects replays the save arrays, so it must keep naming that
// spot, while the overlay is where the host says the element belongs. BindUnboundReCreates then
// searches the host position FIRST -- a churned mirror's surviving actor -- and falls back to
// the save position for a purge re-create, snapping such a re-bind to the host position. (If
// savePos itself were retracked, a purge re-create could never match and the eid would be
// permanently unbindable.)
//
// Returns true and fills `saveOut` with the immutable save position when the pile genuinely
// MOVED, more than 50 cm from it; the caller then arms a host-vacate twin to retire the stale
// copy there. False for a small nudge, which the pos-correction handles alone, or for an eid
// not in the chip identity map. Game thread.
bool UpdateChipHostPos(coop::element::ElementId eid, const ue_wrap::FVector& newPos,
                       ue_wrap::FVector& saveOut);

// DEV PROBE, gated on [dev] `force_save_churn`: deterministically reproduce the precondition
// for the position re-bind. Real engine GC churn is non-deterministic -- a sparse two natives
// out of nearly nine hundred -- so a clean run may never exercise that path at all. This UNBINDS
// the first N currently-bound chipPile save-natives, taking their mirror Element while the actor
// stays alive at its save position, right before the quiescence sweep, so the sweep then sees N
// unbound natives at save positions and re-binds them by the host-wire savePos. One-shot
// (latched). No-op unless the flag is set. Game thread.
void ForceSaveChurnForTest();

// DEV SELF-TEST, gated on [dev] reseed_orphan_selftest=1, one-shot, and runs wherever live
// chipPile natives exist. Reproduces the re-seed-orphan race in-process on a real save-native
// actor, with no join and no rendering, so it cannot pass for the wrong reason. It arms a
// one-entry map at a live native's position, binds it to a free host eid, takes the Element and
// enqueues it deferred -- the reaper's taken-but-not-flushed window -- then runs the flush,
// re-seed and re-bind sequence and asserts the churned native is RE-BOUND to its host eid
// rather than orphaned. Restores the subject native afterwards. Returns true on pass. Requires
// save_identity_bind=1. Game thread.
bool RunReseedOrphanSelfTest();

// CLIENT session end: drop the map, both per-family lists and cursors, and the bound-native
// guard set.
void OnDisconnect();

}  // namespace coop::save_identity_bind
