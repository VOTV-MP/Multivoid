// coop/npc_adoption.h -- v75 deferred CLASS-MATCH adoption of save-persisted NPCs (the kerfur) +
// the post-snapshot ghost sweep. Ground truth:
// research/findings/votv-kerfurOmega-coop-double-and-camera-RE-2026-06-14.md (sec "CORRECTION" + 11).
//
// PROBLEM: a save-persisted NPC (the kerfur) is materialized on a save-transfer joining client by
// the game's native EX_CallMath save-load spawn -- invisible to our PE suppressor, at an
// unpredictable time. The host ALSO broadcasts it as an EntitySpawn (savePersisted=1). If the
// client fresh-spawned a mirror it would DOUBLE with its own local copy. v74 tried to ADOPT the
// local copy by matching the int_save Key, but kerfurOmega::loadData OVERRIDES the int_save base
// and drops the key restore, so each peer's UCS mints a RANDOM key -- key-equality could never
// match (bytecode-proven; this is why v74 "changed nothing").
//
// FIX: ADOPT the client's own local twin by CLASS (the only portable identity -- both peers loaded
// the same save, so the same exact skin class), found by a DEFERRED POLL of GUObjectArray (we
// cannot hook the EX_CallMath spawn). Adopting the REAL local actor (vs a fresh mirror parked at
// spawn, before its `cam` UChildActorComponent child initializes) is also camera-safe: the local
// twin is fully initialized before we park it, so its destroy cascade tears the cam child down like
// the host's. A genuine ORPHAN (an untracked NPC with no host EntitySpawn -- e.g. a kerfur turned
// OFF after the save, so the host's world has only the prop form) is removed by the ghost sweep,
// fired ONCE after the connect snapshot is delivered (SnapshotComplete) AND adoption has converged.
//
// Principle 7: gameplay/network module; engine access via ue_wrap + npc_mirror; GAME THREAD ONLY
// (no mutex -- the pending table + latches are touched only from the net-pump game-thread tick and
// the game-thread reliable-dispatch path).

#pragma once

#include <cstdint>
#include <string>

namespace coop::npc_adoption {

// Record a pending adoption for a save-persisted EntitySpawn (called from npc_mirror::OnEntitySpawn
// when payload.savePersisted==1). Does NOT spawn anything -- Tick() polls for the local twin.
// `actorClass` is the client-resolved + allowlist-validated UClass of `classW`. Idempotent on eid
// (a connect re-announce updates the entry's pose + re-arms the timeout). No-op if eid is already
// bound as a mirror. Game thread.
void ArmAdoption(uint32_t eid, const std::wstring& classW, void* actorClass,
                 float locX, float locY, float locZ,
                 float rotPitch, float rotYaw, float rotRoll);

// Per net-pump tick driver (called from npc_mirror::TickClientNpcs). Two jobs, both NO-OP once
// converged (single integer compares -> zero steady-state cost):
//  (1) THROTTLED (5 Hz while any entry is pending) GUObjectArray scan that binds each pending
//      entry's local twin as a host mirror (class-match: live, allowlisted, untracked, non-CDO,
//      ClassOf==actorClass; nearest to the host pose when several twins exist), parks it host-
//      driven, and -- if no twin appears within the timeout -- fresh-spawns a mirror
//      (npc_mirror::SpawnFreshNpcMirror) so a host NPC is never permanently lost.
//  (2) ONE-SHOT ghost sweep: after the connect snapshot is delivered (OnSnapshotComplete) AND no
//      entries remain pending, fires npc_mirror::DestroyUntrackedClientNpcs() to remove genuine
//      orphans. Gated this way so it NEVER destroys a local twin before its adoption has a chance
//      to bind (the premature-sweep trap).
// Game thread only.
void Tick();

// The host finished delivering the connect snapshot (client-side SnapshotComplete in event_feed).
// ORDERING NOTE: the EntitySpawn handlers are game_thread::Post'd (deferred) while this runs INLINE
// in the same event-drain, so g_pending is EMPTY at this instant and the ArmAdoption tasks are
// still queued. Safety rests on FIFO game-thread ordering: those queued ArmAdoption tasks always
// drain before the NEXT net-pump Tick's ghost-sweep check, and the sweep is gated on g_pending
// being empty -- so the sweep waits out the just-armed adoptions. (This is WHY the sweep lives in
// Tick, never inline here -- see Tick.) Game thread.
//
// Runs for EVERY join incl. live-capture: the ghost sweep (Tick) reconciles the stale
// blob objects the host's live snapshot does not claim, gated on the same load-tail
// quiescence as adoption so a still-loading local twin is adopted, never swept.
void OnSnapshotComplete();

// A fresh connect replay is starting for this client (net_pump, right after it announces
// ClientWorldReady -- which the host answers with a fresh EntitySpawn replay + SnapshotComplete).
// Resets the per-world state: clears stale pending entries from a prior world and re-arms the
// snapshot-delivered + ghost-swept latches, so a save-transfer world swap re-adopts + re-sweeps the
// new world. Game thread.
void OnClientWorldReady();

// Net disconnect (all peers gone): wipe all per-session state.
void OnSessionEnd();

}  // namespace coop::npc_adoption
